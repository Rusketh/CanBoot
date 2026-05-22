#ifndef CANBOOT_SHIM_SYS_SYSINFO_H
#define CANBOOT_SHIM_SYS_SYSINFO_H
/* Bare-metal shim - sysinfo() returns zeroed totals via stubs. */
struct sysinfo {
    long  uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char _f[20 - 2 * sizeof(long) - sizeof(int)];
};
int sysinfo(struct sysinfo *info);
int get_nprocs(void);
#endif
