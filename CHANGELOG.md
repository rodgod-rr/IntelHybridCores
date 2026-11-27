# Changelog

All notable changes to IntelHybridCores. Kernel offsets come from xnu-12377; each hook is installed only if the kernel
instructions match byte for byte.

## [0.3.0] - 2026-11-27

Reference binary for macOS 26.7.3 (25G342).

### Added
- **Throughput option** (`sysctl debug.hc_tput=1/0`, boot-arg `-hctput`; **off by default**). Foreground work takes an
  idle P-core primary thread first, then an E-core, and only then the HyperThreading sibling of a busy P-core, so
  heavily threaded batch work (compiles, renders) uses every physical core while a lone thread still gets an idle
  P-core. Measured on an i5-13450HX (laptop): ~15% faster in short bursts, ~6% at ~30 s, within noise at ~1 min once the
  package is power-limited; no single-task penalty.
- `debug.hc_stat` reset: writing any value (`sysctl debug.hc_stat=0`) zeroes the counters, to measure one workload.
  `hc_stat` shows `tput` while the option is on.

### Documentation
- Working reports: i9-13900H (6P + 8E, [issue #1](https://github.com/rodgod-rr/IntelHybridCores/issues/1)) and
  i7-12700F (8P + 4E, first Alder Lake desktop, [issue #2](https://github.com/rodgod-rr/IntelHybridCores/issues/2)).
- English README as the main one; Portuguese in `README.pt-BR.md`.
- CpuTopologyRebuild: only `ctrsmt=off` works alongside IntelHybridCores (`ctrsmt=full` does not).

## [0.2.0] - 2026-11-21

Reference binary for macOS 26.7.3 (25G342); offsets checked on 26.7.2 (25G332) and 26.7.3 (25G342).

### Added
- **HyperThreading-aware foreground on P-cores** (on by default; `sysctl debug.hc_smt=0/1`, boot-arg `-hcsmtoff`):
  foreground avoids the sibling thread of a busy P-core, like Linux ITMT. HT pairs come from the CPUID core id, since
  the kernel sees each thread as its own core.
- **Real core totals** (`sysctl debug.hc_perf`, boot-arg `hcperf=N`, default 2): `hw.physicalcpu(_max)` and
  `machdep.cpu.cores_per_package` report real cores, `logical_per_package` reports threads; `machdep.cpu.core_count`
  stays from the kernel by default so "About This Mac" keeps the real CPU name.
- **Battery / AC mode switching** (boot-arg `hcpower=B,A`, e.g. `hcpower=4,2`; `sysctl debug.hc_power`): mode B on
  battery, mode A on AC.

## [0.1.0] - 2026-11-15

First release. Offsets checked on macOS 26.7.2 (25G332).

### Added
- **Scheduling by QoS** through the per-pset preferred / migration masks that `choose_processor_smt` already honours:
  background and maintenance work on E-cores, foreground on P-cores. Modes 0–4 (`sysctl debug.hc_mode`, boot-arg
  `hcmode=N`, default 2); mode 4 keeps background strictly on E-cores by also hooking the dualq scheduler
  (`sched_dualq_choose_thread`, `sched_dualq_thread_avoid_processor`).
- `hw.nperflevels = 2` and `hw.perflevel0/1.*` ("Performance" / "Efficiency") with real core counts and caches.
- Activity Monitor CPU History labels ("Core N — Performance / Efficiency").
- Diagnostics: `debug.hc_stat`, `debug.hc_log`; boot-args `-hcoff`, `-hcschedoff`, `-hcperfoff`.

[0.3.0]: https://github.com/rodgod-rr/IntelHybridCores/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/rodgod-rr/IntelHybridCores/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/rodgod-rr/IntelHybridCores/releases/tag/v0.1.0
