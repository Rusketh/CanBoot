/*
 * Minimal inet_pton/inet_ntop for the freestanding build.
 *
 * picolibc ships <arpa/inet.h> with just the byte-order macros, no
 * inet_pton. Mbed TLS's x509 IP SAN parsing path calls inet_pton so we
 * provide our own. Handles AF_INET and AF_INET6.
 */

#include <stdint.h>
#include <string.h>
#include <errno.h>

#ifndef AF_INET
#define AF_INET   2
#endif
#ifndef AF_INET6
#define AF_INET6  10
#endif

static int parse_ipv4(const char *src, unsigned char dst[4]) {
    int saw = 0;
    int oct = 0;
    int v = 0;
    const char *p = src;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            if (v > 255) return 0;
            saw = 1;
        } else if (*p == '.') {
            if (!saw) return 0;
            if (oct >= 3) return 0;
            dst[oct++] = (unsigned char)v;
            v = 0;
            saw = 0;
        } else {
            return 0;
        }
        p++;
    }
    if (oct != 3 || !saw) return 0;
    dst[3] = (unsigned char)v;
    return 1;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static int parse_ipv6(const char *src, unsigned char dst[16]) {
    unsigned char buf[16] = {0};
    int parts[8] = {0};
    int n_parts = 0;
    int dbl = -1;          /* index of "::" location */
    const char *p = src;
    while (*p) {
        if (n_parts == 8 && *p) return 0;
        if (*p == ':') {
            if (n_parts == 0) {
                if (p[1] != ':') return 0;
                p += 2;
                dbl = 0;
                continue;
            }
            if (p[1] == ':') {
                if (dbl != -1) return 0;
                dbl = n_parts;
                p += 2;
                continue;
            }
            return 0;
        }
        int v = 0;
        int chars = 0;
        while (chars < 4 && hex_val(*p) >= 0) {
            v = v * 16 + hex_val(*p);
            p++;
            chars++;
        }
        if (chars == 0) return 0;
        parts[n_parts++] = v;
        if (*p == ':') {
            if (p[1] == 0) return 0;  /* trailing single colon */
        } else if (*p) {
            return 0;
        }
    }

    if (dbl == -1) {
        if (n_parts != 8) return 0;
        for (int i = 0; i < 8; i++) {
            buf[i * 2]     = (unsigned char)(parts[i] >> 8);
            buf[i * 2 + 1] = (unsigned char)(parts[i] & 0xFF);
        }
    } else {
        if (n_parts > 7) return 0;
        int tail = n_parts - dbl;
        for (int i = 0; i < dbl; i++) {
            buf[i * 2]     = (unsigned char)(parts[i] >> 8);
            buf[i * 2 + 1] = (unsigned char)(parts[i] & 0xFF);
        }
        int gap = 8 - n_parts;
        for (int i = 0; i < tail; i++) {
            int idx = dbl + gap + i;
            buf[idx * 2]     = (unsigned char)(parts[dbl + i] >> 8);
            buf[idx * 2 + 1] = (unsigned char)(parts[dbl + i] & 0xFF);
        }
    }
    memcpy(dst, buf, 16);
    return 1;
}

int inet_pton(int af, const char *src, void *dst) {
    if (!src || !dst) {
        errno = EINVAL;
        return -1;
    }
    if (af == AF_INET) {
        return parse_ipv4(src, (unsigned char *)dst);
    }
    if (af == AF_INET6) {
        return parse_ipv6(src, (unsigned char *)dst);
    }
    errno = EAFNOSUPPORT;
    return -1;
}
