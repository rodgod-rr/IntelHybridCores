# IntelHybridCores

**Real P-cores and E-cores on Intel macOS (Alder Lake / Raptor Lake)** · [Português](README.md)

Intel macOS never ran on a hybrid Mac, so it treats every core of an Alder Lake or Raptor Lake chip as equal: background work competes with the foreground app for the P-cores, and no tool shows which cores are the efficiency ones. Yet the Intel kernel already carries, unused, most of what Apple Silicon relies on to tell cores apart. IntelHybridCores is a [Lilu](https://github.com/acidanthera/Lilu) plugin that fills those pieces in.

| What | Result |
|---|---|
| QoS scheduling | Background work (Spotlight, Time Machine, photo analysis) on E-cores; foreground on P-cores |
| `hw.nperflevels` / `hw.perflevel0/1` | `2` / "Performance" and "Efficiency", with real core counts and caches |
| Activity Monitor → CPU History | "Core 1 — Performance" … "Core 13 — Efficiency" |
| P-core HyperThreading | Foreground avoids the sibling thread of a busy P-core (like Linux ITMT) |
| `hw.physicalcpu` / System Information | Real core count (10 instead of 16) and "Hyper-Threading: Enabled" |
| Battery ↔ AC | Mode switches by itself (e.g. 4 on battery, 2 on AC) |

> **Warning:** kernel offsets come from xnu-12377 and were checked on **macOS 26.7.2 (25G332)** and **26.7.3 (25G342)**, on an i5-13450HX (6P + 4E). Each hook is installed only if the kernel instructions match byte for byte; on another macOS version, whatever does not match stays off (see `sysctl debug.hc_log`). Tested here on an i5-13450HX; there is a working report on an i9-13900H (6P + 8E) in [issue #1](https://github.com/rodgod-rr/IntelHybridCores/issues/1). Use at your own risk, with a rescue USB at hand.

## Results (i5-13450HX, macOS 26.7.2)

Usage per core type, measured with [`tools/cpuuse.c`](tools/cpuuse.c) (100% = one core fully busy), idle Mac on AC:

| Scenario | Without IntelHybridCores (P/E) | Mode 2 (P/E) | Mode 4 (P/E) |
|---|---|---|---|
| 4 background loops | 512% / 30% | 141% / 358% | 95% / 400% |
| 8 background loops | 794% / 147% | 522% / 388% | 95% / 400% |
| 4 foreground + 8 background | 982% / 316% | 912% / 374% | 544% / 400% |
| 1 foreground loop | 194% / 0% | 250% / 2% | 192% / 3% |

Without IntelHybridCores the kernel picks the lowest-numbered idle CPU, which is always a P-core; the E-cores barely work.

Power with 8 background loops for 20 s (CPU package):

| | AC | Battery | Work per joule |
|---|---|---|---|
| Mode 2 | 52 W | 30 W | baseline |
| Mode 4 | 27 W | 18 W | ~30% lower |

Mode 4 halves package power under heavy background load (less heat and fan noise, P-cores free), but background work takes longer and uses slightly more energy overall: Intel E-cores at turbo are not nearly as efficient as Apple's. Idle, both modes are equal (~9.5 W on battery).

## Modes

`sysctl debug.hc_mode=N` changes it live; boot-arg `hcmode=N` sets the boot mode (default **2**).

| Mode | Effect |
|---|---|
| 0 | off |
| 1 | background and maintenance prefer E-cores |
| 2 | 1 + everything else prefers P-cores; background work on a P-core moves when an E-core is idle |
| 3 | 2 + `utility` (downloads, exports) prefers E-cores |
| 4 | 3 + background **only** on E-cores, as on Apple Silicon |

Real-time threads (audio, video) are never touched. A background thread promoted above priority 20 (holding a lock a foreground app waits on) escapes mode 4, so that app does not stall.

**Battery ↔ AC:** boot-arg `hcpower=B,A` (e.g. `hcpower=4,2`) uses mode B on battery and A on AC; `sysctl debug.hc_power="4,2"` turns it on at runtime and `"0"` off. The mode changes only when the source changes, so a manual `debug.hc_mode` stays until the next plug/unplug. If mode 4 is not available it falls back to 3.

**HyperThreading:** on by default; `sysctl debug.hc_smt=0/1` at runtime, boot-arg `-hcsmtoff` at boot.

**Core totals** (`sysctl debug.hc_perf`, boot-arg `hcperf=N`):

| Value | `hw.perflevel` and labels | `hw.physicalcpu(_max)` | `machdep.cpu.core_count` / `cores_per_package` / `logical_per_package` |
|---|---|---|---|
| 0 | kernel's | kernel's | kernel's |
| 1 | fixed | real cores | real cores / threads |
| **2** (default) | fixed | real cores | kernel's |
| 3 | fixed | kernel's | real cores / threads |

The default is 2 because of About This Mac (see Pitfalls).

Other boot-args: `-hcoff` (all off), `-hcschedoff` (no scheduler hooks), `-hcperfoff` (no `hw.perflevel`, totals or labels).

## Install

1. Requirements: Lilu 1.6.4 or newer; a hybrid CPU; **CpuTopologyRebuild disabled or with `ctrsmt=off`** (only `off`: neither its default mode nor `ctrsmt=full` works). In those, E-cores become SMT siblings of the P-cores and the preference stops working (IntelHybridCores detects this and skips the scheduler part; `debug.hc_stat` shows `state 6`).
2. Copy `IntelHybridCores.kext` to `EFI/OC/Kexts` and add it to `Kernel → Add` after Lilu.
3. Reboot and check:

```bash
sysctl debug.hc_stat debug.hc_log hw.nperflevels hw.perflevel1.name hw.physicalcpu debug.hc_smt debug.hc_power
```

`state 1` = scheduler hooked; `strict-ok` = mode 4 available; `smt` = HyperThreading handled; `auto` = battery/AC switching on. The counters show how many choices went to the requested type (`bg E/P`, `fg P/E`) and, for foreground on P-cores, how many went to a primary thread (`smt prim`), to a secondary whose sibling was idle (`sec-free`) or busy (`sec-cont`).

Reference binary 0.2.0 (tested on macOS 26.7.3 / 25G342): [`dist/`](dist). Version 0.1.0 (without HyperThreading, totals or `hcpower`) is in the git history. To build: `DEPS=/folder/with/MacKernelSDK+Lilu.kext bash build.sh`.

## How it works (and how to port it to another version)

Everything is in [`src/IntelHybridCores.cpp`](src/IntelHybridCores.cpp). xnu source references: [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu), tag `xnu-12377.121.6`.

### 1. Core type
CPUID `0x1A` on every CPU (`mp_rendezvous_no_intrs`): `0x40` = Core (P), `0x20` = Atom (E). CPUID `0xB` gives the x2APIC id (physical core) and CPUID `4` the caches and how many CPUs share the L2.

### 2. Scheduler
The Intel scheduler is `dualq`, which uses `choose_processor_smt` (`sched_prim.c`). That function already consults two per-pset masks that are always zero on Intel, because only the ARM performance controller writes them:

- `perfcontrol_cpu_preferred_bitmask` (pset+`0x8a0`): among idle CPUs, try these first;
- `perfcontrol_cpu_migration_bitmask` (pset+`0x8a8`): CPUs to avoid, including the thread's last-CPU hint.

The hook fills both from the thread's effective QoS (`proc_get_effective_thread_policy(thread, TASK_POLICY_QOS)`), calls the original and clears them. It runs under the pset lock, and any value is valid for the kernel. To find the offsets on another version: disassemble `_choose_processor_smt` and look for the idle-CPU mask being `and`ed with `0x8a0(%r13)`, followed by `cpu_preferred_last_chosen` (+`0x10`).

Mode 4 needs two more hooks, because an idle P-core pulls from the shared `dualq` run queue without going through CPU selection:

- **`sched_dualq_choose_thread`**: a P-core refuses a background thread from the queue. The function is static; its address comes from the `_sched_dualq_dispatch` table (+`0x30`), validated by `choose_processor` at +`0x58`.
- **`sched_dualq_thread_avoid_processor`** (+`0xf0`): a background thread already running on a P-core is moved off. The kernel requeues it with `SCHED_REBALANCE`.

### 2b. HyperThreading
The kernel builds its topology from the CPUID "per package" fields (leaves 1 and 4). On hybrid chips they hold the maximum addressable ids (`machdep.cpu.logical_per_package` = `cores_per_package` = 128), so the kernel concludes 1 thread per core: every HT thread becomes its own core, primary of itself (`processor_primary`), and `choose_processor_smt` does not know which CPUs are siblings. That topology is built before any kext loads and is left alone. IntelHybridCores groups the P-cores by the CPUID `0xB` core id (the lowest CPU number of each physical core is the primary) and, for foreground work, removes from the preferred mask and adds to the migration mask every secondary whose sibling is dispatching or running (`processor->state` ≥ 5).

### 3. `hw.perflevel` and totals
`kern_mib.c` already registers `hw.perflevel0` **and** `hw.perflevel1`; they only report "one type" because the Intel machine functions say so. IntelHybridCores replaces the `oid_handler` of those nodes (found from `_sysctl__children`) with one that answers per type. The same way it replaces `hw.physicalcpu(_max)` and `machdep.cpu.core_count` / `cores_per_package` / `logical_per_package`, keeping the original handler (used with `debug.hc_perf=0`).

### 4. Activity Monitor
Activity Monitor reads the IODeviceTree `cpus` service and, on each child, `cluster-type` (data `"P"`/`"E"`; byte `'E'` = Efficiency, `'P'` = Performance, `'M'` = a third type) and `logical-cpu-id`. Then it **sorts all children by `logical-cpu-id`**. On a PC the children are the ACPI processors (`C000@0`, `C00C@38`… the unit address is the x2APIC id), and ACPI usually declares more processors than exist; the unused ones get ids 64+ so they sort last.

### 5. Battery ↔ AC
The battery service (`IOPMPowerSource`, e.g. `AppleSmartBattery`) is published once at boot: the publish notification does **not** fire again on plug/unplug. IntelHybridCores keeps the service and reads `ExternalConnected` every 5 s (`thread_call`).

### Pitfalls (each one cost a panic or a failed test)

- **About This Mac picks the CPU name from `machdep.cpu.core_count`.** It combines the SMBIOS processor type with that total and looks it up in a table of CPUs Apple shipped. With 16 "cores" nothing matches and it shows the real CPUID name; with type `0x0F01` (Xeon W, common in OpenCore configs) and 10 cores it matches the iMac Pro's "Xeon W 10-Core". That is why the default (`hc_perf=2`) fixes `hw.physicalcpu`, which programs use, and leaves the kernel's `machdep.cpu.core_count`. Setting `ProcessorType` to a Core i5 code on a MacBookPro16,1 SMBIOS (a model that only had i7/i9) gives an "unknown" CPU.

- **The Tahoe kernel is built with LTO.** `ml_get_cpu_types`, `ml_get_cpu_number_type` and friends were folded into `sysctl_hw_generic` as constants: hooking those functions changes nothing. Hence the sysctl handler swap.
- **Static sysctls live in `__DATA_CONST`**, read-only after boot: writing `oid_handler` directly **panics**. Write through Lilu's `MachInfo::setKernelWriting`.
- **Unused arguments are not passed.** `thread_select` calls `choose_thread` with only `(processor, priority)` because the original ignores the rest; the third argument (`prev`) is garbage.
- **Refusing a thread needs `must_idle`.** If `choose_thread` returns nothing while the queue is non-empty, `thread_select` loops forever. Set `processor->must_idle` (+`0x71`), the same exit the kernel uses for NO_SMT threads.
- **Functions starting with a relative jump** (`ml_cpu_get_info_type`, `ml_cpu_cache_sharing`, `ml_get_cpu_number_type`) do not work with Lilu's trampoline.
- **In zsh, `log` is a builtin.** To read the system log, use `/usr/bin/log show`.
- Logs from kexts injected by OpenCore often do not reach `log show`; IntelHybridCores keeps its last messages in `sysctl debug.hc_log`.

## License and credits

MIT, © 2026 Rodrigo Lacerda Rangel Santos. Uses [Lilu](https://github.com/acidanthera/Lilu) and [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) (acidanthera). Kernel names and structures come from the open-source [xnu](https://github.com/apple-oss-distributions/xnu). Not affiliated with Apple or Intel.
