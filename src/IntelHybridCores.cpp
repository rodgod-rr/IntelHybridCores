//  IntelHybridCores: P-core / E-core awareness for macOS on Intel hybrid CPUs (Alder Lake, Raptor Lake).
//  Copyright (c) 2026 Rodrigo Lacerda Rangel Santos. MIT License (see LICENSE).
//
//  The Intel build of XNU never shipped on a hybrid Mac, so it treats every CPU of an Alder/Raptor Lake chip as equal.
//  It already contains, unused, most of what Apple Silicon uses to tell cores apart; this Lilu plugin fills it in:
//
//  1. Scheduling by QoS (like Apple Silicon / Linux ITMT). choose_processor_smt (sched_prim.c) already honours two
//     per-pset masks that only the ARM performance controller writes (zero on Intel):
//       perfcontrol_cpu_preferred_bitmask  idle CPUs in this mask are tried first
//       perfcontrol_cpu_migration_bitmask  CPUs to avoid (including the thread's last-CPU hint)
//     The hook fills both from the thread's effective QoS, calls the original and clears them (pset lock is held).
//     Strict mode (4) also hooks the dualq scheduler's choose_thread (a P-core does not take a background thread from
//     the shared run queue: returns THREAD_NULL with must_idle, the exit the kernel itself uses for NO_SMT threads) and
//     thread_avoid_processor (a background thread running on a P-core is moved off).
//  2. hw.nperflevels = 2 and hw.perflevel0/1.* ("Performance" / "Efficiency"). kern_mib.c registers both nodes, but LTO
//     folded the single-type x86 machine functions into sysctl_hw_generic, so the oid_handler of those nodes is replaced
//     (the nodes live in __DATA_CONST: written with Lilu's kernel write enabled).
//  3. Activity Monitor labels (CPU History: "Core N - Performance/Efficiency"): it reads IODeviceTree "cpus" children
//     "cluster-type" ("P"/"E") and "logical-cpu-id", sorting all children by the id. The children here are the ACPI
//     processors (unit address = x2APIC id); unused ACPI processors get ids 64+ so they sort last.
//
//  Modes (boot-arg hcmode=N, default 2; sysctl debug.hc_mode at runtime):
//    0 off   1 background/maintenance prefer E   2 = 1 + everything else prefers P (background on a P-core moves to an
//    idle E-core)   3 = 2 + utility prefers E   4 = 3 + background STRICTLY on E (as on Apple Silicon)
//  Real-time threads are never touched. A background thread promoted above priority 20 (holding a lock someone waits
//  for) escapes strict mode to avoid priority inversion.
//
//  SMT awareness (P-cores with HyperThreading): when choosing a CPU for foreground work, P-core secondaries whose
//  sibling is busy are deprioritized (added to the migration mask). This avoids placing two foreground threads on the
//  same physical P-core (sharing execution resources via HT), similar to Linux ITMT's per-core priority ranking.
//  Stats: smt prim = foreground→P-primary, sec-free = →P-secondary (sibling idle), sec-cont = →P-secondary (contended).
//
//  Boot-args: -hcoff (plugin off), -hcschedoff (no scheduler hooks), -hcperfoff (no hw.perflevel / labels),
//  -hcsmtoff (no SMT-aware foreground scheduling on P-cores with HyperThreading).
//  hcpower=bat,ac (e.g. hcpower=4,2): auto-switch mode by power source (battery→4, AC→2).
//  sysctl: debug.hc_mode, debug.hc_stat, debug.hc_perf (1/0 at runtime), debug.hc_smt (1/0),
//  debug.hc_power ("bat,ac" to enable, "0" to disable), debug.hc_log.
//
//  Every kernel offset below comes from xnu-12377 and was checked on macOS 26.7.2 (25G332). Each hook is installed only
//  if the instructions that use those offsets are found byte for byte; otherwise that part stays off (see debug.hc_log).
//  Requires E-cores to be SMT primaries: with CpuTopologyRebuild's default mode (E-cores reported as SMT siblings of the
//  P-cores) the scheduler part disables itself.
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IONotifier.h>
#include <kern/thread_call.h>
#include <sys/sysctl.h>

// ---- log: last lines kept for sysctl debug.hc_log (IOLog of injected kexts often never reaches `log show`)
static char gLog[1536];
static void hcLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void hcLog(const char *fmt, ...) {
	char line[200];
	va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
	SYSLOG("hc", "%s", line);
	size_t used = strlen(gLog), add = strlen(line) + 1;
	if (used + add >= sizeof(gLog)) { gLog[0] = 0; used = 0; }
	strlcat(gLog, line, sizeof(gLog)); strlcat(gLog, "\n", sizeof(gLog));
}

// ---- kernel layout (xnu-12377, macOS 26.7.2), verified by instruction bytes at install time
static constexpr uint32_t kPrefOff = 0x8a0, kMigOff = 0x8a8;            // pset: preferred / migration bitmask
static constexpr uint32_t kSchedPriOff = 0x434, kThBoundProc = 0x4b0;   // thread: sched_pri, bound_processor
static constexpr uint32_t kProcState = 0x00, kProcPset = 0x28, kProcCurPri = 0x30, kCpuIdOff = 0x48, kProcMustIdle = 0x71;
static constexpr uint32_t kProcBoundRq = 0x268, kProcPrimary = 0x618;   // processor: bound run queue, processor_primary
static constexpr uint32_t kPsetMainRq = 0x110, kRqHighq = 0x00, kRqCount = 0x18, kRqQueues = 0x20;   // run_queue
static constexpr int kQosPolicy = 0x35;     // TASK_POLICY_QOS (policy_internal.h)
static constexpr int kRtPri = 97;           // BASEPRI_RTQUEUES
static constexpr int kStrictMaxPri = 20;    // background/maintenance base priority is 4; above 20 = promoted

static mach_vm_address_t gChOrg = 0, gCtOrg = 0, gAvOrg = 0;
static int (*gQos)(void *thread, int flavor) = nullptr;
static void **gProcArray = nullptr;
static int (*gCpuNumber)() = nullptr;
static volatile int gMode = 2;
static uint64_t gPMask = 0, gEMask = 0;
static uint32_t gState = 0;     // 1 ok, 2 symbols, 3 kernel layout differs, 4 not hybrid / CPU types, 5 route, 6 E not primary
static bool gStrictOk = false;
static uint64_t gSt[9] {};      // bg E/P, fg P/E, utility E/P, strict: refused/moved/swapped
static uint64_t gSmtSt[3] {};   // fg: P-prim, P-sec (free sibling), P-sec (contended)
static uint64_t gPPrimMask = 0, gPSecMask = 0;
static uint8_t gSib[64] {};      // HT sibling (same CPUID core id) of each P-secondary
static bool gSmtAware = false;
static volatile int gSmtOn = 1;

// ---- power source auto-mode: batMode on battery, acMode on AC (IOPMPowerSource notification)
static volatile int gBatMode = 4, gAcMode = 2;
static bool gPowerAuto = false;
static IONotifier *gPwrNotifier = nullptr;

// ---- per-CPU probe (CPUID on every CPU, interrupts off)
static uint8_t gCoreType[64] {};
static uint32_t gApic[64] {}, gCoreId[64] {}, gL2Id[64] {}, gCache[64][4] {};   // L1i, L1d, L2, L3 bytes
static inline void cpuidx(uint32_t l, uint32_t s, uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d) {
	asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(l), "c"(s));
}
static uint32_t shiftFor(uint32_t n) { uint32_t s = 0; while ((1u << s) < n) s++; return s; }
static void hcProbe(void *) {
	const int n = gCpuNumber();
	if (n < 0 || n >= 64) return;
	uint32_t a, b, c, d;
	cpuidx(0x1a, 0, a, b, c, d);
	gCoreType[n] = (uint8_t)(a >> 24);            // 0x40 Core (P), 0x20 Atom (E)
	cpuidx(0xb, 0, a, b, c, d);
	gApic[n] = d; gCoreId[n] = d >> (a & 0x1f);   // x2APIC, physical core id
	for (uint32_t i = 0; i < 8; i++) {
		cpuidx(4, i, a, b, c, d);
		const uint32_t type = a & 0x1f, level = (a >> 5) & 7;
		if (!type) break;
		const uint32_t size = ((b >> 22) + 1) * (((b >> 12) & 0x3ff) + 1) * ((b & 0xfff) + 1) * (c + 1);
		if (level == 1) gCache[n][type == 2 ? 0 : 1] = size;
		else if (level == 2) { gCache[n][2] = size; gL2Id[n] = gApic[n] >> shiftFor(((a >> 14) & 0xfff) + 1); }
		else if (level == 3) gCache[n][3] = size;
	}
}

template <typename T> static inline T fld(void *p, uint32_t off) { return *reinterpret_cast<volatile T *>((uint8_t *)p + off); }
static inline bool isPCpu(void *proc) { const uint32_t id = fld<uint32_t>(proc, kCpuIdOff); return id < 64 && ((gPMask >> id) & 1); }

// thread class in the current mode: 1 wants E, 2 wants P, 0 no rule
static inline int hcClass(int qos, int mode) {
	switch (qos) {
	case 1: case 2: return 1;                                     // MAINTENANCE, BACKGROUND
	case 3: return mode >= 3 ? 1 : 0;                             // UTILITY
	case 0: case 4: case 5: case 6: return mode >= 2 ? 2 : 0;     // unspecified, LEGACY, USER_INITIATED, USER_INTERACTIVE
	default: return 0;
	}
}
static inline bool strictBg(void *th) {
	if (fld<int16_t>(th, kSchedPriOff) >= kStrictMaxPri) return false;
	const int q = gQos(th, kQosPolicy);
	return q == 1 || q == 2;
}
static void *bestECpu() {   // idle E-core first, else the one running the lowest priority
	void *best = nullptr; int bestPri = 1 << 30;
	for (uint64_t m = gEMask; m; m &= m - 1) {
		void *p = gProcArray[__builtin_ctzll(m)];
		if (!p) continue;
		const int st = fld<int>(p, kProcState);
		if (st == 4) return p;                 // PROCESSOR_IDLE
		if (st != 5 && st != 6) continue;      // DISPATCHING, RUNNING
		const int pri = fld<int>(p, kProcCurPri);
		if (pri < bestPri) { bestPri = pri; best = p; }
	}
	return best;
}

// ---- hooks
static void *chHook(void *pset, void *processor, void *thread, void *options) {   // choose_processor_smt
	const int mode = gMode;
	if (mode <= 0 || fld<int16_t>(thread, kSchedPriOff) >= kRtPri) return FunctionCast(chHook, gChOrg)(pset, processor, thread, options);
	const int qos = gQos(thread, kQosPolicy), cls = hcClass(qos, mode);
	if (!cls) return FunctionCast(chHook, gChOrg)(pset, processor, thread, options);
	auto pref = reinterpret_cast<volatile uint64_t *>((uint8_t *)pset + kPrefOff);
	auto mig = reinterpret_cast<volatile uint64_t *>((uint8_t *)pset + kMigOff);
	if (cls == 1) {
		*pref = gEMask;
		*mig = gPMask;
	} else if (gSmtAware && gSmtOn) {
		// SMT-aware foreground: prefer all P, avoid E + P-secondaries whose sibling is busy (contended HT)
		uint64_t avoid = gEMask;
		for (uint64_t m = gPSecMask; m; m &= m - 1) {
			const uint32_t s = (uint32_t)__builtin_ctzll(m);
			void *sp = gProcArray[s];
			if (!sp) continue;
			void *pr = gProcArray[gSib[s]];
			if (pr && fld<int>(pr, kProcState) >= 5) avoid |= 1ULL << s;   // sibling dispatching/running
		}
		*pref = gPMask & ~avoid;   // contended P-secondaries leave the preferred set too
		*mig = avoid;
	} else {
		*pref = gPMask;
		*mig = gEMask;
	}
	void *r = FunctionCast(chHook, gChOrg)(pset, processor, thread, options);
	*pref = 0; *mig = 0;
	if (r && mode >= 4 && gStrictOk && cls == 1 && qos != 3 && isPCpu(r) && strictBg(thread))
		if (void *e = bestECpu()) { r = e; gSt[8]++; }
	if (r) {
		const uint32_t id = fld<uint32_t>(r, kCpuIdOff);
		const bool isE = id < 64 && ((gEMask >> id) & 1);
		gSt[(cls == 2 ? 2 : qos == 3 ? 4 : 0) + (cls == 2 ? isE : !isE)]++;
		if (cls == 2 && gSmtAware && gSmtOn && !isE && id < 64) {
			if ((gPPrimMask >> id) & 1) gSmtSt[0]++;
			else {
				void *pr = gProcArray[gSib[id]];
				gSmtSt[pr && fld<int>(pr, kProcState) >= 5 ? 2 : 1]++;
			}
		}
	}
	return r;
}
// sched_dualq_choose_thread(processor, priority): thread_select passes only rdi/esi (the original ignores the rest)
static void *ctHook(void *processor, int priority, void *prev, int reason) {
	if (gMode >= 4 && isPCpu(processor) && fld<int>(processor, kProcBoundRq + kRqCount) == 0) {
		uint8_t *rq = fld<uint8_t *>(processor, kProcPset) + kPsetMainRq;
		const int hq = fld<int>(rq, kRqHighq);
		if (fld<int>(rq, kRqCount) > 0 && hq >= priority && hq >= 0 && hq < 128) {
			void *head = fld<void *>(rq, kRqQueues + (uint32_t)hq * 8);   // runq_links is the first field of a thread
			if (head && strictBg(head)) {
				*reinterpret_cast<volatile bool *>((uint8_t *)processor + kProcMustIdle) = true;   // or thread_select spins
				gSt[6]++;
				return nullptr;
			}
		}
	}
	return FunctionCast(ctHook, gCtOrg)(processor, priority, prev, reason);
}
static bool avHook(void *processor, void *thread, int reason) {   // sched_dualq_thread_avoid_processor
	if (FunctionCast(avHook, gAvOrg)(processor, thread, reason)) return true;
	if (gMode < 2 || !isPCpu(processor) || fld<void *>(thread, kThBoundProc) == processor || !strictBg(thread)) return false;
	if (gMode >= 4) { gSt[7]++; return true; }
	for (uint64_t m = gEMask; m; m &= m - 1) {   // modes 2/3: only when an E-core is idle
		void *e = gProcArray[__builtin_ctzll(m)];
		if (e && fld<int>(e, kProcState) == 4) { gSt[7]++; return true; }
	}
	return false;
}

// ---- hw.perflevel
static volatile int gPerfOn = 1;
static uint32_t gTyLog[2] {}, gTyPhys[2] {}, gTyL2Share[2] {}, gTyCache[2][4] {};
static sysctl_handler_t gHwGeneric = nullptr;
static constexpr int kHwNPerf = 10 | (1 << 30), kHwPerfFlag = 1 << 29;   // HW_NPERFLEVELS, CTLHW_PERFLEVEL (kern_mib.c)
static int perfHandler SYSCTL_HANDLER_ARGS {
	if (!gPerfOn) return gHwGeneric(oidp, arg1, arg2, req);
	if (arg2 == kHwNPerf) return sysctl_handle_int(oidp, nullptr, 2, req);
	if (!(arg2 & kHwPerfFlag)) return gHwGeneric(oidp, arg1, arg2, req);
	const int t = (int)(intptr_t)arg1 == 1 ? 1 : 0;
	int v;
	switch (arg2 & 0xff) {
	case 11: case 12: v = (int)gTyPhys[t]; break;          // physicalcpu(_max)
	case 13: case 14: v = (int)gTyLog[t]; break;           // logicalcpu(_max)
	case 15: v = (int)gTyCache[t][0]; break;               // l1icachesize
	case 16: v = (int)gTyCache[t][1]; break;               // l1dcachesize
	case 17: v = (int)gTyCache[t][2]; break;               // l2cachesize
	case 18: v = (int)gTyL2Share[t]; break;                // cpusperl2
	case 19: v = (int)gTyCache[t][3]; break;               // l3cachesize
	case 20: v = (int)(gTyLog[0] + gTyLog[1]); break;      // cpusperl3
	case 21: { char nm[16]; strlcpy(nm, t ? "Efficiency" : "Performance", sizeof(nm)); return sysctl_handle_string(oidp, nm, 0, req); }
	default: return gHwGeneric(oidp, arg1, arg2, req);
	}
	return sysctl_handle_int(oidp, nullptr, v, req);
}

// ---- sysctls
static int modeSysctl SYSCTL_HANDLER_ARGS {
	int v = gMode; const int e = sysctl_handle_int(oidp, &v, 0, req);
	if (!e && req->newptr) {
		if (gState != 1) return EINVAL;
		gMode = v < 0 ? 0 : v > 4 ? 4 : v;
		if (gMode == 4 && !gStrictOk) gMode = 3;
		hcLog("mode %d", gMode);
	}
	return e;
}
SYSCTL_PROC(_debug, OID_AUTO, hc_mode, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, nullptr, 0, modeSysctl, "I", "");
static int statSysctl SYSCTL_HANDLER_ARGS {
	char s[384];
	snprintf(s, sizeof(s), "state %u%s%s mode %d%s P 0x%llx E 0x%llx | bg E/P %llu/%llu | fg P/E %llu/%llu | utility E/P %llu/%llu | strict refused %llu moved %llu swapped %llu | smt prim %llu sec-free %llu sec-cont %llu",
	         gState, gStrictOk ? " strict-ok" : "", gSmtAware ? (gSmtOn ? " smt" : " smt-off") : "",
	         gMode, gPowerAuto ? " auto" : "", gPMask, gEMask, gSt[0], gSt[1], gSt[2], gSt[3], gSt[4], gSt[5], gSt[6], gSt[7], gSt[8], gSmtSt[0], gSmtSt[1], gSmtSt[2]);
	return sysctl_handle_string(oidp, s, 0, req);
}
SYSCTL_PROC(_debug, OID_AUTO, hc_stat, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, nullptr, 0, statSysctl, "A", "");
static int perfSysctl SYSCTL_HANDLER_ARGS {
	int v = gPerfOn; const int e = sysctl_handle_int(oidp, &v, 0, req);
	if (!e && req->newptr) gPerfOn = v ? 1 : 0;
	return e;
}
SYSCTL_PROC(_debug, OID_AUTO, hc_perf, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, nullptr, 0, perfSysctl, "I", "");
static int logSysctl SYSCTL_HANDLER_ARGS { return sysctl_handle_string(oidp, gLog, 0, req); }
SYSCTL_PROC(_debug, OID_AUTO, hc_log, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, nullptr, 0, logSysctl, "A", "");
static int smtSysctl SYSCTL_HANDLER_ARGS {
	int v = gSmtOn; const int e = sysctl_handle_int(oidp, &v, 0, req);
	if (!e && req->newptr) { gSmtOn = v ? 1 : 0; hcLog("smt %s", gSmtOn ? "on" : "off"); }
	return e;
}
SYSCTL_PROC(_debug, OID_AUTO, hc_smt, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, nullptr, 0, smtSysctl, "I", "");
// ---- power source: the battery service is published once at boot (the publish notification does NOT fire again on
// plug/unplug), so it is kept retained and its ExternalConnected is polled every 5 s. The mode is changed only when the
// source changes (or auto is turned on), so a manual debug.hc_mode stays until the next plug/unplug.
static IOService *gPwrSrc = nullptr;
static thread_call_t gPwrTc = nullptr;
static volatile int gLastAC = -1;
static void pwrCheck() {
	if (!gPowerAuto || gState != 1 || !gPwrSrc) return;
	auto conn = OSDynamicCast(OSBoolean, gPwrSrc->getProperty("ExternalConnected"));
	if (!conn) return;
	const int ac = conn->isTrue() ? 1 : 0;
	if (ac == gLastAC) return;
	gLastAC = ac;
	int m = ac ? gAcMode : gBatMode;
	m = m < 0 ? 0 : m > 4 ? 4 : m;
	if (m == 4 && !gStrictOk) m = 3;
	gMode = m;
	hcLog("power %s -> mode %d", ac ? "AC" : "battery", gMode);
}
static void pwrTick(thread_call_param_t, thread_call_param_t) {
	pwrCheck();
	uint64_t dl; clock_interval_to_deadline(5, kSecondScale, &dl); thread_call_enter_delayed(gPwrTc, dl);
}
// power auto-mode: "bat,ac" e.g. "4,2" or "0" to disable
static int powerSysctl SYSCTL_HANDLER_ARGS {
	char buf[16];
	snprintf(buf, sizeof(buf), gPowerAuto ? "%d,%d" : "0", gBatMode, gAcMode);
	const int e = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (!e && req->newptr) {
		int b, a;
		if (buf[0] == '0' && (buf[1] == 0 || buf[1] == '\n')) {
			gPowerAuto = false;
			hcLog("power auto off");
		} else if (sscanf(buf, "%d,%d", &b, &a) == 2 && b >= 0 && b <= 4 && a >= 0 && a <= 4) {
			gBatMode = b; gAcMode = a; gLastAC = -1; gPowerAuto = true;
			hcLog("power auto bat=%d ac=%d", gBatMode, gAcMode);
			pwrCheck();
		}
	}
	return e;
}
SYSCTL_PROC(_debug, OID_AUTO, hc_power, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, nullptr, 0, powerSysctl, "A", "");

static bool pwrSourcePublished(void *, void *, IOService *svc, IONotifier *) {
	if (!svc || gPwrSrc) return true;
	svc->retain(); gPwrSrc = svc;
	hcLog("power: watching %s", svc->getName());
	if (gPwrTc) thread_call_enter(gPwrTc);
	return true;
}

static bool hasBytes(mach_vm_address_t fn, size_t len, const uint8_t *pat, size_t n) {
	auto p = reinterpret_cast<const uint8_t *>(fn);
	for (size_t i = 0; i + n <= len; i++) if (!memcmp(p + i, pat, n)) return true;
	return false;
}

// ---- Activity Monitor labels (10 s after start: the ACPI processor nubs do not exist yet when the patcher loads)
static void dtLabel(thread_call_param_t, thread_call_param_t) {
	auto cpus = IORegistryEntry::fromPath("/cpus", gIODTPlane);
	if (!cpus) { hcLog("labels: IODeviceTree:/cpus not found"); return; }
	uint32_t extra = 64, marked = 0;
	auto it = cpus->getChildIterator(gIODTPlane);
	while (auto c = it ? OSDynamicCast(IORegistryEntry, it->getNextObject()) : nullptr) {
		const char *loc = c->getLocation();
		int cpu = -1;
		if (loc && *loc) {
			uint32_t apic = 0; bool ok = true;
			for (const char *q = loc; *q; q++) {
				const char h = *q; uint32_t v;
				if (h >= '0' && h <= '9') v = (uint32_t)(h - '0'); else if (h >= 'a' && h <= 'f') v = (uint32_t)(h - 'a' + 10);
				else if (h >= 'A' && h <= 'F') v = (uint32_t)(h - 'A' + 10); else { ok = false; break; }
				apic = apic * 16 + v;
			}
			for (uint32_t n = 0; ok && n < 64; n++)
				if ((((gPMask | gEMask) >> n) & 1) && gApic[n] == apic) { cpu = (int)n; break; }
		}
		if (cpu >= 0) {
			if (auto ct = OSData::withBytes((gPMask >> cpu) & 1 ? "P" : "E", 2)) { c->setProperty("cluster-type", ct); ct->release(); }
			marked++;
		}
		if (auto num = OSNumber::withNumber(cpu >= 0 ? (uint32_t)cpu : extra++, 32)) { c->setProperty("logical-cpu-id", num); num->release(); }
	}
	OSSafeReleaseNULL(it);
	cpus->release();
	hcLog("labels: %u CPUs marked in IODeviceTree:/cpus (+%u unused ACPI processors moved last)", marked, extra - 64);
}

static sysctl_oid *oidFind(sysctl_oid_list *l, const char *name) {
	if (!l) return nullptr;
	for (sysctl_oid *o = SLIST_FIRST(l); o; o = SLIST_NEXT(o, oid_link)) if (o->oid_name && !strcmp(o->oid_name, name)) return o;
	return nullptr;
}

static void perfInstall(KernelPatcher &patcher) {
	if (checkKernelArgument("-hcperfoff")) return;
	for (int t = 0; t < 2; t++) {
		const uint64_t m = t == 0 ? gPMask : gEMask;
		uint32_t cores[64], l2s[64], nc = 0, nl = 0;
		for (uint32_t i = 0; i < 64; i++) {
			if (!((m >> i) & 1)) continue;
			gTyLog[t]++;
			bool dc = false, dl = false;
			for (uint32_t k = 0; k < nc; k++) if (cores[k] == gCoreId[i]) dc = true;
			for (uint32_t k = 0; k < nl; k++) if (l2s[k] == gL2Id[i]) dl = true;
			if (!dc) cores[nc++] = gCoreId[i];
			if (!dl) l2s[nl++] = gL2Id[i];
			if (!gTyCache[t][0]) for (int c = 0; c < 4; c++) gTyCache[t][c] = gCache[i][c];
		}
		gTyPhys[t] = nc;
		gTyL2Share[t] = nl ? gTyLog[t] / nl : 1;
	}
	if (!gTyLog[0] || !gTyLog[1] || !gTyPhys[0] || !gTyPhys[1] || !gTyCache[0][1] || !gTyCache[1][1]) { hcLog("perflevel: incomplete topology"); return; }
	auto root = reinterpret_cast<sysctl_oid_list *>(patcher.solveSymbol(KernelPatcher::KernelID, "_sysctl__children")); patcher.clearError();
	sysctl_oid *hw = oidFind(root, "hw");
	if (!hw || (hw->oid_kind & CTLTYPE) != CTLTYPE_NODE) { hcLog("perflevel: hw node not found"); return; }
	auto hwl = static_cast<sysctl_oid_list *>(hw->oid_arg1);
	sysctl_oid *np = oidFind(hwl, "nperflevels"), *pl[2] {oidFind(hwl, "perflevel0"), oidFind(hwl, "perflevel1")};
	if (!np || np->oid_arg2 != kHwNPerf || !np->oid_handler || !pl[0] || !pl[1]) { hcLog("perflevel: nodes differ"); return; }
	gHwGeneric = np->oid_handler;
	sysctl_oid *todo[40]; int nt = 0;
	for (int lv = 0; lv < 2; lv++)
		for (sysctl_oid *o = SLIST_FIRST(static_cast<sysctl_oid_list *>(pl[lv]->oid_arg1)); o; o = SLIST_NEXT(o, oid_link)) {
			if (o->oid_handler != gHwGeneric || !(o->oid_arg2 & kHwPerfFlag) || (int)(intptr_t)o->oid_arg1 != lv || nt >= 39) {
				hcLog("perflevel: child %s differs", o->oid_name ? o->oid_name : "?"); return;
			}
			todo[nt++] = o;
		}
	todo[nt++] = np;
	// static sysctl oids are in __DATA_CONST (read-only after boot): a plain store panics
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) { hcLog("perflevel: kernel write denied"); return; }
	for (int i = 0; i < nt; i++) todo[i]->oid_handler = perfHandler;
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	sysctl_register_oid(&sysctl__debug_hc_perf);
	if (auto tc = thread_call_allocate(dtLabel, nullptr)) { uint64_t dl; clock_interval_to_deadline(10, kSecondScale, &dl); thread_call_enter_delayed(tc, dl); }
	hcLog("perflevel: %d nodes; P %u log/%u phys L1d %u L2 %u x%u, E %u log/%u phys L1d %u L2 %u x%u", nt,
	      gTyLog[0], gTyPhys[0], gTyCache[0][1], gTyCache[0][2], gTyL2Share[0], gTyLog[1], gTyPhys[1], gTyCache[1][1], gTyCache[1][2], gTyL2Share[1]);
}

static void schedInstall(KernelPatcher &patcher, mach_vm_address_t fn) {
	if (checkKernelArgument("-hcschedoff")) { hcLog("scheduler: off (-hcschedoff)"); return; }
	static const uint8_t pPref[] {0x49, 0x8b, 0x95, 0xa0, 0x08, 0x00, 0x00};                      // movq 0x8a0(%r13),%rdx
	static const uint8_t pMig[] {0x49, 0x8b, 0x8d, 0xa8, 0x08, 0x00, 0x00};                       // movq 0x8a8(%r13),%rcx
	static const uint8_t pPri[] {0x66, 0x41, 0x83, 0xbc, 0x24, 0x34, 0x04, 0x00, 0x00, 0x61};     // cmpw $0x61,0x434(%r12)
	static const uint8_t pPrim[] {0x4c, 0x8b, 0xb6, 0x18, 0x06, 0x00, 0x00};                      // movq 0x618(%rsi),%r14
	if (!hasBytes(fn, 0x400, pPref, sizeof(pPref)) || !hasBytes(fn, 0x400, pMig, sizeof(pMig)) ||
	    !hasBytes(fn, 0x400, pPri, sizeof(pPri)) || !hasBytes(fn, 0x40, pPrim, sizeof(pPrim))) {
		gState = 3; hcLog("scheduler: choose_processor_smt differs from xnu-12377 / 26.7.2, not hooked"); return;
	}
	// the masks only act on SMT primaries: E-cores reported as SMT siblings (CpuTopologyRebuild default) defeat them
	for (uint64_t m = gEMask; m; m &= m - 1) {
		void *p = gProcArray[__builtin_ctzll(m)];
		if (p && fld<void *>(p, kProcPrimary) != p) { gState = 6; hcLog("scheduler: E-core cpu %d is an SMT secondary (CpuTopologyRebuild?), not hooked", __builtin_ctzll(m)); return; }
	}
	// SMT topology from CPUID (the kernel may see every HT thread as its own core, hw.physicalcpu == logicalcpu, so
	// processor_primary is useless): in each physical P-core (same gCoreId) the lowest CPU number is the primary.
	if (!checkKernelArgument("-hcsmtoff")) {
		for (uint32_t i = 0; i < 64; i++) {
			if (!((gPMask >> i) & 1) || !gProcArray[i]) continue;
			int prim = -1;
			for (uint32_t j = 0; j < i; j++)
				if (((gPMask >> j) & 1) && gProcArray[j] && gCoreId[j] == gCoreId[i]) { prim = (int)j; break; }
			if (prim < 0) gPPrimMask |= 1ULL << i;
			else { gPSecMask |= 1ULL << i; gSib[i] = (uint8_t)prim; }
		}
		gSmtAware = gPPrimMask != 0 && gPSecMask != 0;
		hcLog("SMT: P-prim 0x%llx P-sec 0x%llx%s", gPPrimMask, gPSecMask, gSmtAware ? "" : " (no HT pairs, off)");
	}
	gChOrg = patcher.routeFunction(fn, reinterpret_cast<mach_vm_address_t>(chHook), true);
	if (patcher.getError() != KernelPatcher::Error::NoError) { gChOrg = 0; gState = 5; patcher.clearError(); hcLog("scheduler: route failed"); return; }
	// strict mode: choose_thread (+0x30) and thread_avoid_processor (+0xf0) of the dualq table; choose_processor (+0x58)
	// pointing at choose_processor_smt confirms the table layout. Both are static and called directly (devirtualized),
	// so the function bodies are routed.
	auto tbl = reinterpret_cast<mach_vm_address_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_sched_dualq_dispatch")); patcher.clearError();
	if (tbl && tbl[0x58 / 8] == fn) {
		const mach_vm_address_t ct = tbl[0x30 / 8], av = tbl[0xf0 / 8];
		static const uint8_t c1[] {0x4c, 0x8b, 0x47, 0x28, 0x49, 0x8d, 0xb8, 0x10, 0x01, 0x00, 0x00, 0x8b, 0x91, 0x68, 0x02, 0x00, 0x00};
		static const uint8_t c2[] {0x83, 0x7f, 0x18, 0x00};             // cmpl $0,0x18(%rdi)      run_queue.count
		static const uint8_t c3[] {0x48, 0x8b, 0x44, 0xc7, 0x20};       // movq 0x20(%rdi,%rax,8)  run_queue.queues[highq]
		static const uint8_t c4[] {0xc6, 0x41, 0x71, 0x01};             // movb $1,0x71(%rcx)      processor.must_idle
		static const uint8_t a1[] {0x4c, 0x8b, 0xb6, 0xb0, 0x04, 0x00, 0x00};   // movq 0x4b0(%rsi),%r14  thread.bound_processor
		static const uint8_t p1[] {0x8b, 0x46, 0x30};                   // movl 0x30(%rsi),%eax    processor.current_pri
		if (ct && av && hasBytes(ct, 0x20, c1, sizeof(c1)) && hasBytes(ct, 0x140, c2, sizeof(c2)) && hasBytes(ct, 0x140, c3, sizeof(c3)) &&
		    hasBytes(ct, 0x140, c4, sizeof(c4)) && hasBytes(av, 0x20, a1, sizeof(a1)) && hasBytes(fn, 0x400, p1, sizeof(p1))) {
			gCtOrg = patcher.routeFunction(ct, reinterpret_cast<mach_vm_address_t>(ctHook), true);
			if (patcher.getError() == KernelPatcher::Error::NoError) {
				gAvOrg = patcher.routeFunction(av, reinterpret_cast<mach_vm_address_t>(avHook), true);
				if (patcher.getError() == KernelPatcher::Error::NoError) gStrictOk = true;
			}
			patcher.clearError();
		}
	} else patcher.clearError();
	if (!gStrictOk) hcLog("scheduler: strict mode unavailable (table/signature/route)");
	int m = 2;
	if (PE_parse_boot_argn("hcmode", &m, sizeof(m))) gMode = m < 0 ? 0 : m > 4 ? 4 : m;
	if (gMode == 4 && !gStrictOk) gMode = 3;
	gState = 1;
	if (gSmtAware) sysctl_register_oid(&sysctl__debug_hc_smt);
	hcLog("scheduler: hooked, mode %d%s%s", gMode, gStrictOk ? ", strict available" : "", gSmtAware ? ", SMT aware" : "");
}

static void hcInstall(KernelPatcher &patcher) {
	sysctl_register_oid(&sysctl__debug_hc_mode); sysctl_register_oid(&sysctl__debug_hc_stat); sysctl_register_oid(&sysctl__debug_hc_log);
	uint32_t a, b, c, d;
	cpuidx(7, 0, a, b, c, d);
	if (!((d >> 15) & 1)) { gState = 4; hcLog("not a hybrid CPU (CPUID.07H:EDX[15] = 0), nothing to do"); return; }
	auto K = KernelPatcher::KernelID;
	mach_vm_address_t fn = patcher.solveSymbol(K, "_choose_processor_smt"); patcher.clearError();
	gQos = reinterpret_cast<int (*)(void *, int)>(patcher.solveSymbol(K, "_proc_get_effective_thread_policy")); patcher.clearError();
	gProcArray = reinterpret_cast<void **>(patcher.solveSymbol(K, "_processor_array")); patcher.clearError();
	gCpuNumber = reinterpret_cast<int (*)()>(patcher.solveSymbol(K, "_cpu_number")); patcher.clearError();
	auto rdv = reinterpret_cast<void (*)(void (*)(void *), void *)>(patcher.solveSymbol(K, "_mp_rendezvous_no_intrs")); patcher.clearError();
	if (!fn || !gQos || !gProcArray || !gCpuNumber || !rdv) { gState = 2; hcLog("kernel symbols not found"); return; }
	rdv(hcProbe, nullptr);
	for (uint32_t i = 0; i < 64; i++) {
		if (!gProcArray[i]) continue;
		if (fld<uint32_t>(gProcArray[i], kCpuIdOff) != i) { gState = 3; hcLog("processor_array[%u].cpu_id differs: kernel layout unknown", i); return; }
		if (gCoreType[i] == 0x40) gPMask |= 1ULL << i;
		else if (gCoreType[i] == 0x20) gEMask |= 1ULL << i;
	}
	if (!gPMask || !gEMask) { gState = 4; hcLog("CPU types: P 0x%llx E 0x%llx, need both", gPMask, gEMask); return; }
	hcLog("CPUs: P 0x%llx E 0x%llx", gPMask, gEMask);
	schedInstall(patcher, fn);
	perfInstall(patcher);
	// power auto-mode: boot-arg hcpower=bat,ac (e.g. hcpower=4,2) enables auto-switching by power source
	char pw[16] {};
	if (PE_parse_boot_argn("hcpower", pw, sizeof(pw))) {
		int pb, pa;
		if (sscanf(pw, "%d,%d", &pb, &pa) == 2 && pb >= 0 && pb <= 4 && pa >= 0 && pa <= 4) {
			gBatMode = pb; gAcMode = pa; gPowerAuto = true;
			hcLog("power auto: bat=%d ac=%d", gBatMode, gAcMode);
		}
	}
	sysctl_register_oid(&sysctl__debug_hc_power);
	if (gState == 1) {
		auto dict = IOService::serviceMatching("IOPMPowerSource");
		if (dict) {
			gPwrTc = thread_call_allocate(pwrTick, nullptr);
			gPwrNotifier = IOService::addMatchingNotification(
				gIOPublishNotification, dict, pwrSourcePublished, nullptr);
			dict->release();
		}
	}
}

static const char *bootargOff[] {"-hcoff"};
static const char *bootargDebug[] {"-hcdbg"};
PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal,
	bootargOff, arrsize(bootargOff),
	bootargDebug, arrsize(bootargDebug),
	nullptr, 0,
	KernelVersion::Tahoe,
	KernelVersion::Tahoe,
	[]() { lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) { hcInstall(patcher); }); },
};
