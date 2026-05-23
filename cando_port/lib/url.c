/*
 * cando url module - URL parser helpers. The parse helpers are
 * stateless: each takes the same URL string and returns the
 * requested component. Cheap to implement, easy to call from cando
 * without object plumbing.
 *
 *   url.scheme(s)   "https" / "http" / null
 *   url.host(s)     hostname or IPv4 literal
 *   url.port(s)     numeric port, or 80/443 default for http/https
 *   url.path(s)     path-and-query starting with "/", or "/" if absent
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "core/value.h"
#include "vm/vm.h"
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "lib/libutil.h"
#include "lib/object.h"

struct url_parts {
    const char *scheme;   size_t scheme_n;
    const char *host;     size_t host_n;
    int         port;
    const char *path;     size_t path_n;
};

static int parse(const char *url, struct url_parts *p) {
    if (!url) return -1;
    const char *s = url;
    /* scheme */
    const char *colon = strstr(s, "://");
    if (!colon) return -1;
    p->scheme = s;
    p->scheme_n = (size_t)(colon - s);
    s = colon + 3;
    /* host[:port] */
    const char *host_start = s;
    while (*s && *s != ':' && *s != '/') s++;
    p->host = host_start;
    p->host_n = (size_t)(s - host_start);
    p->port = 0;
    if (*s == ':') {
        s++;
        int n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
        p->port = n;
    }
    if (p->port == 0) {
        if (p->scheme_n == 5 && memcmp(p->scheme, "https", 5) == 0) p->port = 443;
        else                                                        p->port = 80;
    }
    /* path */
    if (*s == '\0' || *s == '/') {
        p->path = s[0] ? s : "/";
        p->path_n = s[0] ? strlen(s) : 1;
    } else {
        p->path = "/";
        p->path_n = 1;
    }
    return 0;
}

static int u_scheme(CandoVM *vm, int argc, CandoValue *args) {
    struct url_parts p;
    if (parse(libutil_arg_cstr_at(args, argc, 0), &p) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *s = cando_string_new(p.scheme, (uint32_t)p.scheme_n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int u_host(CandoVM *vm, int argc, CandoValue *args) {
    struct url_parts p;
    if (parse(libutil_arg_cstr_at(args, argc, 0), &p) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *s = cando_string_new(p.host, (uint32_t)p.host_n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static int u_port(CandoVM *vm, int argc, CandoValue *args) {
    struct url_parts p;
    if (parse(libutil_arg_cstr_at(args, argc, 0), &p) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    cando_vm_push(vm, cando_number((f64)p.port));
    return 1;
}

static int u_path(CandoVM *vm, int argc, CandoValue *args) {
    struct url_parts p;
    if (parse(libutil_arg_cstr_at(args, argc, 0), &p) != 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CandoString *s = cando_string_new(p.path, (uint32_t)p.path_n);
    cando_vm_push(vm, cando_string_value(s));
    return 1;
}

static const LibutilMethodEntry url_methods[] = {
    { "scheme", u_scheme },
    { "host",   u_host   },
    { "port",   u_port   },
    { "path",   u_path   },
};

void canboot_cando_open_urllib(CandoVM *vm) {
    CandoValue obj_val = cando_bridge_new_object(vm);
    CdoObject *obj     = cando_bridge_resolve(vm, cando_as_handle(obj_val));
    libutil_register_methods(vm, obj, url_methods,
                             sizeof(url_methods) / sizeof(url_methods[0]));
    cando_vm_set_global(vm, "url", obj_val, true);
}
