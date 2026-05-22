#ifndef CANBOOT_SHIM_TERMIOS_H
#define CANBOOT_SHIM_TERMIOS_H
/* Bare-metal shim. cando's lib/console_term.c uses termios for raw-mode
 * tty manipulation; we wrap our hal/console raw mode via stubs in
 * cando_stubs.c so tcsetattr/tcgetattr just succeed without effect. */

typedef unsigned int  tcflag_t;
typedef unsigned int  speed_t;
typedef unsigned char cc_t;

#define NCCS 32

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

#define ICANON  0000002
#define ECHO    0000010
#define ECHONL  0000100
#define ISIG    0000001
#define IEXTEN  0100000
#define IXON    0002000
#define ICRNL   0000400
#define INLCR   0000100
#define IGNCR   0000200
#define INPCK   0000020
#define ISTRIP  0000040
#define PARMRK  0000010
#define PARENB  0000400
#define BRKINT  0000002
#define IGNBRK  0000001
#define OPOST   0000001
#define CS8     0000060
#define CSIZE   0000060

#define VMIN    6
#define VTIME   5

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int act, const struct termios *t);
#endif
