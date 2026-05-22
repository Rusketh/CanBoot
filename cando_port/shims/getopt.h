#ifndef CANBOOT_SHIM_GETOPT_H
#define CANBOOT_SHIM_GETOPT_H

/* Bare-metal getopt shim. canboot drives mkntfs via a pre-populated
 * struct mkntfs_options - we never need to parse argv. The shim
 * supplies the type and constants the upstream source references,
 * plus getopt_long stubs that immediately report "done" so the parse
 * loop falls through. */

struct option {
    const char *name;
    int         has_arg;
    int        *flag;
    int         val;
};

#define no_argument        0
#define required_argument  1
#define optional_argument  2

extern char *optarg;
extern int   optind;
extern int   opterr;
extern int   optopt;

int getopt(int argc, char *const argv[], const char *optstring);
int getopt_long(int argc, char *const argv[], const char *optstring,
                 const struct option *longopts, int *longindex);

#endif
