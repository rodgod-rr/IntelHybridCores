// Per-CPU usage over an interval (host_processor_info), summed by core type (P mask from IntelHybridCores' debug.hc_stat).
// No sudo.  clang -O1 -o cpuuse tools/cpuuse.c && ./cpuuse 5 [P mask]
//   output: "P xx% E yy% | per CPU: ..." (100% = one CPU fully busy)
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>

static int snap(unsigned *n, unsigned long long (*t)[CPU_STATE_MAX]) {
	processor_info_array_t a; mach_msg_type_number_t c;
	if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, n, &a, &c) != KERN_SUCCESS) return -1;
	for (unsigned i = 0; i < *n && i < 64; i++)
		for (int s = 0; s < CPU_STATE_MAX; s++) t[i][s] = (unsigned)a[i * CPU_STATE_MAX + s];
	vm_deallocate(mach_task_self(), (vm_address_t)a, c * sizeof(int));
	return 0;
}

int main(int argc, char **argv) {
	double secs = argc > 1 ? atof(argv[1]) : 5;
	unsigned long long pmask = 0;
	if (argc > 2) pmask = strtoull(argv[2], 0, 0);
	else {   // "state 1 ... P 0xfff E 0xf000 | ..."
		char s[320] = {0}; size_t l = sizeof(s) - 1;
		if (!sysctlbyname("debug.hc_stat", s, &l, 0, 0)) { char *p = strstr(s, " P 0x"); if (p) pmask = strtoull(p + 3, 0, 16); }
	}
	unsigned n1, n2; static unsigned long long a[64][CPU_STATE_MAX], b[64][CPU_STATE_MAX];
	if (snap(&n1, a)) return 1;
	usleep((useconds_t)(secs * 1e6));
	if (snap(&n2, b)) return 1;
	double pu = 0, eu = 0; char det[1024] = {0};
	for (unsigned i = 0; i < n2 && i < 64; i++) {
		unsigned long long tot = 0, busy;
		for (int s = 0; s < CPU_STATE_MAX; s++) tot += b[i][s] - a[i][s];
		busy = tot - (b[i][CPU_STATE_IDLE] - a[i][CPU_STATE_IDLE]);
		double u = tot ? 100.0 * busy / tot : 0;
		if ((pmask >> i) & 1) pu += u; else eu += u;
		char x[16]; snprintf(x, sizeof(x), "%s%.0f", i ? " " : "", u); strcat(det, x);
	}
	printf("P %5.0f%% E %5.0f%% | per CPU: %s\n", pu, eu, det);
	return 0;
}
