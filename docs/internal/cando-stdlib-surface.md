# CanDo stdlib API surface

This document inventories the public API surface of every CanDo stdlib library, extracted from headers and implementations. It drives the CanBoot stdlib rewrite by identifying conventions, CLASS construction patterns, and platform dependencies.

## Conventions

### Error model
**Throw-based.** Native functions return `-1` from `CandoNativeFn` (signature: `int fn(CandoVM *vm, int argc, CandoValue *args)`) to signal an error; the VM's error message is populated via `cando_vm_error(vm, fmt, ...)`. Some high-level wrappers (file.read, net.lookup) return `null` on benign failures (file not found, host not resolved) rather than throw. Return value convention: functions return positive integers to indicate the number of values pushed onto the VM stack (typically 1, sometimes 0 for side-effect-only calls). Callbacks invoked from C code must not leave exceptions set.

### Async model
**Callback-based with thread workers.** Socket server (`socket.createServer(callback)`), HTTP server, and `console.start()` spawn background threads that invoke user callbacks inside child VMs. The child VM is initialized via `cando_vm_init_child(parent)` so it shares the parent's handle table and globals. No promises/futures; async I/O is explicitly threaded.

### Object model
**CLASS instances (cando.h terminology: CdoObject).** Built-in types (sockets, streams, HTTP responses, servers) wrap C state in a CdoObject with hidden fields (e.g., `__socket_id` holding a slot index) and a meta table (e.g., `_meta.tcp_socket`) that serves as the prototype. Method dispatch uses Lua-style `__index` chaining. The `_meta` global is a writable registry; users can add/override methods on built-in types.

### EventEmitter equivalent
**No global EventEmitter pattern; callbacks only.** Thread libraries and console use direct callback registration (`thread.then(t, fn)`, `console:onKey(fn)`). HTTP servers and socket servers use a single user-provided callback per listener, not a per-event subscription model.

### Buffer / bytes type
**String (native Cando string type).** All byte I/O uses CandoString; encoding is specified as an optional parameter ("utf8" / "binary" / "hex" / "base64" / "base64url"). The `stream` library unifies byte-oriented I/O behind a vtable-backed handle pattern.

### Timer lib
**No dedicated timer library.** OS clock functions (`os.time()` → seconds since epoch; `os.clock()` → CPU seconds) and sleep (`thread.sleep(ms)`) exist. No setTimeout/setInterval; async operations use threads + callbacks.

## Bridge entry points

To construct CLASS instances with bound methods (e.g., to create a Socket or Stream), CanBoot's port must call:

### Object allocation and handle tracking
- **`cando_bridge_new_object(CandoVM *vm) → CandoValue`** (`/home/user/CanBoot/vendor/cando/source/vm/bridge.c:30`)
  — Allocate a plain CdoObject and register a handle. Returns a TYPE_OBJECT CandoValue.
  
- **`cando_bridge_new_array(CandoVM *vm) → CandoValue`** (`bridge.c:36`)
  — Allocate an array and register a handle.
  
- **`cando_bridge_track_obj(CandoVM *vm, CdoObject *obj) → HandleIndex`** (`bridge.h:52`)
  — Register a pre-allocated CdoObject in the handle table. Called after the object is populated with fields.

### Meta table attachment
- **`cando_lib_meta_attach(CandoVM *vm, CdoObject *instance, const char *name)`** (`/home/user/CanBoot/vendor/cando/source/lib/meta.c`)
  — Set `instance.__index = _meta.<name>`. Used by socket, stream, HTTP, and all built-in type creators to wire up method dispatch.
  
- **`cando_lib_meta_table(CandoVM *vm, const char *name) → CdoObject *`** (`meta.h:42`)
  — Retrieve (or create) the meta table for a named type. Returns a CdoObject that serves as the prototype.

### Method registration
- **`libutil_set_method(CandoVM *vm, CdoObject *obj, const char *name, CandoNativeFn fn)`** (`/home/user/CanBoot/vendor/cando/source/lib/libutil.c`)
  — Register a single native function as a method on an object. Standard pattern for building module globals.
  
- **`libutil_register_methods(CandoVM *vm, CdoObject *obj, const LibutilMethodEntry *entries, usize count)`** (`libutil.h:188`)
  — Bulk registration of methods from a static table. Reduces boilerplate in library registration.

### Value conversion
- **`cando_bridge_to_cando(CandoVM *vm, CdoValue v) → CandoValue`** (`bridge.c:46`)
  — Convert an object-layer CdoValue to a VM-layer CandoValue. Allocates handles for object subtypes.
  
- **`cando_bridge_to_cdo(CandoVM *vm, CandoValue v) → CdoValue`** (`bridge.h:64`)
  — Reverse: convert CandoValue back to object-layer representation.

### String interning
- **`cando_bridge_intern_key(CandoString *cs) → CdoString *`** (`bridge.h:41`)
  — Intern a CandoString for use as an object field key. Returns a reference-counted CdoString; caller must `cdo_string_release()`.

---

## Per-lib surfaces

### file
- **Namespace:** `file`
- **Bare-metal blockers:** POSIX file I/O (open/read/write/stat/mkdir/opendir), FILE* streams
- **Functions:**
  - `file.read(path, encoding?) → string | null` — Read entire file. Returns null on open/read failure.
  - `file.write(path, data, encoding?) → bool` — Write entire file, overwrite. Returns false on open/write failure.
  - `file.append(path, data, encoding?) → bool` — Append to file. Returns false on open/write failure.
  - `file.exists(path) → bool` — Check if file/dir exists via access(F_OK).
  - `file.delete(path) → bool` — Unlink file. Returns false on failure.
  - `file.copy(src, dst) → bool` — Copy file, fail if dst exists.
  - `file.move(src, dst) → bool` — Rename file.
  - `file.size(path) → number | null` — Byte size via stat(). Returns null on stat failure.
  - `file.lines(path, encoding?) → array | null` — Read file as array of strings (one per line).
  - `file.mkdir(path) → bool` — Create directory via mkdir(). Returns false on failure.
  - `file.list(path) → array | null` — List directory entries via opendir/readdir. Returns null on failure.
  - `file.basename(path) → string` — Extract filename (last path component).
  - `file.dirname(path) → string` — Extract directory path.
  - `file.extname(path) → string` — Extract file extension.
  - `file.join(...paths) → string` — Join path components.
  - `file.resolve(...paths) → string` — Resolve relative path to absolute via realpath().
  - `file.realpath(path) → string | null` — Canonicalize path. Returns null on failure.
- **CLASSes returned:** None directly; internal `file.stream` returns a Stream object.
- **Events emitted:** None.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/file.h`, `file.c`

### net
- **Namespace:** `net`
- **Bare-metal blockers:** POSIX getaddrinfo()
- **Functions:**
  - `net.lookup(host) → array` — Resolve hostname to array of IP strings (numeric form, no further DNS). Returns empty array on failure (does not throw).
- **CLASSes returned:** None.
- **Events emitted:** None.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/net.h`, `net.c`

### socket
- **Namespace:** `socket`
- **Bare-metal blockers:** POSIX sockets (socket/connect/listen/accept/send/recv), getaddrinfo, select/poll for timeouts
- **Functions:**
  - `socket.tcp() → tcp_socket` — Allocate unconnected TCP socket object.
  - `socket.connect(host, port [, opts]) → tcp_socket` — Allocate and connect in one call. Convenience wrapper.
  - `socket.createServer(callback) → tcp_server` — Create TCP listener. Callback signature: `callback(conn)` where `conn` is a connected socket object. Server spawns background accept thread; callback runs in child VMs.
  - `socket.resolve(host) → array` — Resolve hostname to array of IP strings via getaddrinfo (dual-stack).
- **CLASSes returned:**
  - `tcp_socket` (attached to `_meta.tcp_socket`):
    - `:connect(host, port [, opts]) → undefined` — Connect to remote peer. Throws on failure.
    - `:send(data) → number` — Send bytes, return count sent. Throws on error.
    - `:sendAll(data) → undefined` — Send all bytes, loop until done or error. Throws on error.
    - `:recv(maxLen) → string` — Receive up to maxLen bytes. Returns empty string on clean EOF (not an error).
    - `:recvAll() → string` — Receive until EOF.
    - `:recvLine() → string` — Receive until CRLF or LF; return stripped. Empty string on EOF.
    - `:close() → undefined` — Close socket. Idempotent.
    - `:isOpen() → bool` — True if socket not closed.
    - `:setTimeout(ms) → undefined` — Set read/write timeout in milliseconds.
    - `:setBlocking(bool) → undefined` — Set blocking mode.
    - `:setOption(name, value) → undefined` — Set socket option (e.g., "TCP_NODELAY", "SO_KEEPALIVE").
    - `:fd() → number` — Get underlying file descriptor (for debugging/FFI).
    - `:localAddress() → { host, port }` — Get local sockaddr.
    - `:remoteAddress() → { host, port }` — Get peer sockaddr.
    - `:stream([caps]) → stream` — Wrap socket in Stream adapter.
  - `tcp_server` (attached to `_meta.tcp_server`):
    - `:listen(port [, host]) → undefined` — Bind and start accepting. Spawns accept thread; returns immediately.
    - `:close() → undefined` — Stop listener and close socket. Idempotent.
    - `:fd() → number` — Get underlying listener socket fd.
    - `:localAddress() → { host, port }` — Get bound address.
- **Events emitted:** None (callbacks only; per-connection callback is passed the connected socket).
- **Error convention:** Programmer errors (wrong types, calling :send on closed socket) and I/O failures throw via `cando_vm_error`. Clean EOF on `:recv` returns empty string (not an error).
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/socket.h`, `socket.c`

### secure_socket
- **Namespace:** `secure_socket`
- **Bare-metal blockers:** OpenSSL (SSL_CTX, SSL_connect/accept, X.509 verification), POSIX sockets (same as socket)
- **Functions:**
  - `secure_socket.tcp([opts]) → tls_socket` — Allocate unconnected TLS socket.
  - `secure_socket.connect(host, port [, opts]) → tls_socket` — Allocate and TLS-connect.
  - `secure_socket.createServer(opts, callback) → tls_server` — Create TLS listener. `opts = {cert, key, verifyPeer?, ca?}`. Callback runs in child VM per accepted connection.
- **CLASSes returned:**
  - `tls_socket` (attached to `_meta.tls_socket`): Same methods as `tcp_socket` above, plus:
    - `:getPeerCertificate() → { subject, issuer, validFrom, validTo, fingerprint, ...}` — X.509 cert introspection (OpenSSL ASN.1 names parsed to objects).
  - `tls_server` (attached to `_meta.tls_server`): Same as `tcp_server`.
- **Events emitted:** None.
- **Error convention:** Handshake failures, verification failures, and I/O errors throw.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/secure_socket.h`, `secure_socket.c`

### sockutil
- **Namespace:** None (internal; no script-visible API)
- **Bare-metal blockers:** POSIX sockets, OpenSSL, getaddrinfo, SIGPIPE handling
- **Functions (C API only, not exposed to scripts):**
  - `sockutil_tcp_connect(host, port, family, timeout_ms, err, errlen) → sockutil_socket_t` — Low-level TCP connect.
  - `sockutil_tcp_listen(host, port, family, backlog, err, errlen) → sockutil_socket_t` — Low-level TCP listen.
  - `sockutil_tcp_accept(listen_fd, timeout_ms, peer_addr, peer_len, err, errlen) → sockutil_socket_t` — Accept one connection.
  - `sockutil_send_raw(fd, buf, len) → int` — Send bytes (return: >0 bytes sent, -1 error).
  - `sockutil_recv_raw(fd, buf, len) → int` — Receive bytes (return: >0 bytes recv, 0 EOF, -1 error).
  - `sockutil_tls_wrap(fd, ctx, is_client, sni_host, err, errlen) → SSL *` — Wrap socket with TLS.
  - `sockutil_build_client_ssl_ctx(opts, err, errlen) → SSL_CTX *` — Build client TLS context.
  - `sockutil_build_server_ssl_ctx(cert_pem, cert_len, key_pem, key_len, opts, err, errlen) → SSL_CTX *` — Build server TLS context.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/sockutil.h`, `sockutil.c`

### http
- **Namespace:** `http` (also global function `fetch()`)
- **Bare-metal blockers:** POSIX sockets, OpenSSL (TLS for https links), getaddrinfo, HTTP/1.1 parsing
- **Functions:**
  - `http.request(opts) → http_response` — Full-featured HTTP client. `opts = { url, method?, headers?, body?, timeout?, ... }`. Returns response object with status, headers, body.
  - `http.get(url) → http_response` — Convenience GET wrapper.
  - `http.createServer(callback) → http_server` — Create HTTP listener. `callback(req, res)`. Server spawns accept thread; callback runs in child VM per connection.
  - `fetch(url [, opts]) → http_response` — Global function. Auto-detects http vs https from URL scheme.
- **CLASSes returned:**
  - `http_response` (attached to `_meta.http_response`):
    - `:status() → number` — Get HTTP status code.
    - `:headers() → object` — Get response headers (lowercased keys).
    - `:body() → string` — Get response body.
  - `http_request` (attached to `_meta.http_request`): TBD — read `http.c` for request object layout.
  - `http_server` (attached to `_meta.http_server`):
    - `:listen(port [, host]) → undefined` — Bind and start accepting. Spawns accept thread.
    - `:close() → undefined` — Stop listener.
- **Events emitted:** None (callbacks only).
- **Error convention:** Network errors, timeouts, malformed responses throw.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/http.h`, `http.c`

### https
- **Namespace:** `https`
- **Bare-metal blockers:** OpenSSL, POSIX sockets
- **Functions:**
  - `https.request(opts) → http_response` — HTTPS variant of http.request (same opts).
  - `https.get(url) → http_response` — HTTPS convenience GET.
  - `https.createServer(opts, callback) → https_server` — HTTPS server with TLS. `opts = {cert, key, verifyPeer?, ca?}`.
- **CLASSes returned:** Same as http (http_response, https_server on `_meta.https_server`).
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/https.h`, `https.c`

### httputil
- **Namespace:** None (internal C API for http/https shared infrastructure)
- **Bare-metal blockers:** OpenSSL, POSIX sockets
- **Structures and functions (C API only):**
  - `HttpBuf` — Dynamic byte buffer (data, len, cap).
  - `HttpUrl` — Parsed URL (scheme, host, port, path).
  - `HttpHeaders` — Ordered header list with name/value pairs.
  - `http_parse_url(url, out) → bool` — Parse URL string.
  - `http_read_request(conn, r) → bool` — Server: parse incoming HTTP request.
  - `http_read_response(conn, r) → bool` — Client: parse HTTP response.
  - `http_body_reader_*` — Streaming body parser (supports Content-Length and chunked Transfer-Encoding).
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/httputil.h`, `httputil.c`

### crypto
- **Namespace:** `crypto`
- **Bare-metal blockers:** OpenSSL (libcrypto)
- **Functions:**
  - Hashing: `crypto.md5(data, enc?) → string`, `crypto.sha1(...)`, `crypto.sha256(...)`, `crypto.sha384(...)`, `crypto.sha512(...)`, `crypto.sha3_256(...)`, `crypto.blake2b(...)`, etc.
  - HMAC: `crypto.hmac(digest, key, data, enc?) → string` (digest = "sha256" etc.)
  - KDF: `crypto.pbkdf2(password, salt, iterations, keylen, digest?, enc?) → string`, `crypto.scrypt(...)`, `crypto.hkdf(...)`
  - Random: `crypto.randomBytes(len, enc?) → string`, `crypto.randomInt(min, max) → number`, `crypto.randomUUID() → string`
  - Symmetric ciphers: `crypto.encrypt(algo, key, data, iv?, enc?) → string` (algo = "aes-256-cbc" etc.), `crypto.decrypt(algo, key, data, iv?, enc?) → string`
  - Asymmetric: `crypto.generateKeyPair(type, opts?) → {publicKey, privateKey}`, `crypto.sign(algo, key, data, enc?) → string`, `crypto.verify(algo, key, sig, data) → bool`, `crypto.publicEncrypt(key, data, enc?) → string`, `crypto.privateDecrypt(key, data, enc?) → string`, `crypto.diffieHellman(otherPubKey) → string`
  - X.509: `crypto.x509.create(opts) → cert_pem`, `crypto.x509.parse(cert_pem) → {subject, issuer, ...}`, `crypto.x509.verify(cert, caBundle) → bool`, `crypto.x509.fingerprint(cert, algo?) → string`, `crypto.x509.csr(opts) → csr_pem`
  - Encoding: `crypto.hex.encode(data) → string`, `crypto.hex.decode(hex) → string`, `crypto.base64.encode(data) → string`, `crypto.base64.decode(b64) → string`, `crypto.base64url.encode(...)`, `crypto.base64url.decode(...)`
  - Utilities: `crypto.timingSafeEqual(a, b) → bool`
- **Return convention:** All functions accept optional final `encoding` parameter ("hex", "base64", "base64url", "bytes"). Default is "hex" for hashes; output is a string in the specified format.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/crypto.h`, `crypto.c`

### os
- **Namespace:** `os`
- **Bare-metal blockers:** POSIX getenv/setenv, time(), clock(), getenv, uname
- **Functions:**
  - `os.getenv(name) → string | null` — Get environment variable. Returns null if not set.
  - `os.setenv(name, val) → bool` — Set environment variable. Returns false on failure.
  - `os.execute(command) → number` — Execute shell command via system(). Returns exit code.
  - `os.exit(code) → never` — Terminate process with exit code.
  - `os.time() → number` — Epoch seconds (time()).
  - `os.clock() → number` — CPU seconds (clock() / CLOCKS_PER_SEC).
  - `os.name → string` — Platform name ("unix", "windows", "darwin", etc.).
  - `os.hostname() → string | null` — gethostname(). TBD: confirm error handling.
  - `os.tmpdir() → string` — System temp directory path.
  - `os.homedir() → string` — User home directory path.
  - `os.arch() → string` — CPU architecture ("x86_64", "arm64", etc.).
  - `os.platform() → string` — OS platform ("linux", "macos", "win32", etc.).
  - `os.cpus() → number` — Number of CPUs.
  - `os.freemem() → number` — Free memory bytes.
  - `os.totalmem() → number` — Total memory bytes.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/os.h`, `os.c`

### process
- **Namespace:** `process`
- **Bare-metal blockers:** POSIX getpid/getppid, fork/exec or CreateProcess, pipes
- **Functions:**
  - `process.pid() → number` — Current process ID.
  - `process.ppid() → number` — Parent process ID.
  - `process.spawn(cmd, args?, opts?) → process_handle` — Spawn subprocess. TBD: read `process.c` for full signature and opts.
  - `process.env() → object` — Environment variables as object. TBD: confirm API.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/process.h`, `process.c`

### thread
- **Namespace:** `thread`
- **Bare-metal blockers:** POSIX pthreads or Windows threads, mutexes, condition variables
- **Functions:**
  - `thread.sleep(ms) → undefined` — Sleep current thread for milliseconds.
  - `thread.id() → number` — Numeric ID of current thread.
  - `thread.done(t) → bool` — True if thread t has completed.
  - `thread.join(t) → ...` — Block until thread t finishes; return its result values.
  - `thread.cancel(t) → undefined` — Mark thread t as cancelled (best-effort).
  - `thread.state(t) → string` — State: "pending", "running", "done", "error", "cancelled".
  - `thread.error(t) → value | null` — Return error value if state is "error", else null.
  - `thread.current() → thread_handle | null` — Current thread handle (null on main thread).
  - `thread.then(t, fn) → undefined` — Register success callback; fires with return values.
  - `thread.catch(t, fn) → undefined` — Register error callback; fires with error value.
  - **Language form:** `thread { ... }` — Spawn child thread executing block. Returns thread handle.
  - **Language form:** `await expr` — Wait for thread/promise synchronously.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/thread.h`, `thread.c`

### stream
- **Namespace:** `stream`
- **Bare-metal blockers:** POSIX file I/O, sockets, pipes (depending on adapter)
- **Functions:**
  - `stream.memory(initialBytes?) → stream` — Create duplex in-memory buffer stream.
  - `stream.file(path, mode) → stream` — Create file stream (read/write/append). TBD: confirm signature.
  - `stream.socket(socket_obj) → stream` — Wrap a socket as a stream.
  - `stream.process(process_handle) → stream` — Get stdin/stdout/stderr as streams. TBD: confirm API.
- **CLASSes returned:**
  - `stream` (attached to `_meta.stream`):
    - `:read(maxLen) → string` — Read up to maxLen bytes. Returns empty string on EOF.
    - `:readAll() → string` — Drain to EOF.
    - `:write(data) → number` — Write data, return bytes consumed.
    - `:writeAll(data) → undefined` — Write all bytes, loop until done.
    - `:flush() → undefined` — Flush internal buffers (adapter-defined; may be no-op).
    - `:end() → undefined` — Half-close write side (idempotent).
    - `:close() → undefined` — Full close (idempotent).
    - `:isClosed() → bool` — True if closed.
    - `:error() → string | null` — Get last error message.
    - `:bytesIn() → number` — Bytes read (advisory counter).
    - `:bytesOut() → number` — Bytes written.
    - `:kind() → string` — Stream type ("memory", "file", "tcp", etc.).
    - `:pipe(dst, chunk?) → number` — Drain src into dst in chunk-sized hops. Returns bytes copied.
- **Events emitted:** None (callbacks only; data is polled via `:read()`).
- **Adapter vtable (internal):**
  - `StreamVTable` struct with read/write/flush/end/destroy/seek/tell function pointers.
  - Used to plug in file, socket, HTTP body, process pipe, thread channel adapters.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/stream.h`, `stream.c`

### datetime
- **Namespace:** `datetime`
- **Bare-metal blockers:** POSIX time functions (time, localtime, gmtime, strftime)
- **Functions:**
  - `datetime.now() → number` — Current epoch seconds (same as os.time()).
  - `datetime.format(ts, fmt) → string` — Format timestamp using strftime format string.
  - `datetime.parse(str, fmt) → number | null` — Parse timestamp from string using strftime format. Returns null on parse failure.
  - `datetime.getFullYear(ts) → number` — Year component.
  - `datetime.getMonth(ts) → number` — Month (0-11).
  - `datetime.getDate(ts) → number` — Day of month (1-31).
  - `datetime.getHours(ts) → number` — Hours (0-23).
  - `datetime.getMinutes(ts) → number` — Minutes (0-59).
  - `datetime.getSeconds(ts) → number` — Seconds (0-59).
  - TBD: read docs for full date math API.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/datetime.h`, `datetime.c`

### console
- **Namespace:** `console`
- **Bare-metal blockers:** POSIX termios + ANSI escapes or Windows console API
- **Functions:**
  - **Output:** `console.write(text) → undefined`, `console.print(text) → undefined`, `console.moveCursor(x, y) → undefined`, `console.setColor(fg, bg?) → undefined`, `console.clear() → undefined`, `console.scroll(n) → undefined`
  - **Mode:** `console.rawMode(on) → undefined`, `console.echo(on) → undefined`, `console.cursorVisible(on) → undefined`, `console.alternateScreen(on) → undefined`, `console.enableMouse(on) → undefined`, `console.size() → {cols, rows}`, `console.isatty() → bool`, `console.title(str) → undefined`
  - **Input:** `console.readKey() → string` (blocking), `console.readKeyTimeout(ms) → string | null`, `console.pollKey() → string | null` (non-blocking), `console.readMouse() → {x, y, button, action, ctrl, alt, shift}`, `console.readLine(prompt?, password?) → string`, `console.flushInput() → undefined`
  - **Async:** `console.start() → undefined` (spawn dispatcher thread), `console.stop() → undefined`, `console.wait(timeout_ms?) → undefined` (block until event), `console.running() → bool`
  - **Handlers:** `console.onKey(fn) → undefined`, `console.onMouse(fn) → undefined`, `console.onResize(fn) → undefined`, `console.onLine(fn) → undefined`, `console.onError(fn) → undefined`
  - **Lifecycle:** `console.enable() → undefined`, `console.disable() → undefined`, `console.enabled() → bool`, `console.exists() → bool`, `console.attach() → bool`, `console.detach() → undefined`, `console.hide() → undefined`, `console.show() → undefined`
- **CLASSes returned:** None (console is a global object with methods).
- **Events emitted:** Handlers (onKey, onMouse, onResize, onLine, onError) are called from dispatcher thread; event form is `{kind, key: {name, ctrl, alt, shift}, mouse: {x, y, button, action}, ...}`. TBD: confirm exact event shapes by reading `console_input.h`.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/console.h`, `console.c`

### console_dispatch
- **Namespace:** None (internal; used by console.c)
- **Bare-metal blockers:** POSIX pthreads or Windows threads
- **Functions (C API only):**
  - `console_dispatch_create(parent_vm, console_handle) → ConsoleDispatch *`
  - `console_dispatch_start(d) → undefined`
  - `console_dispatch_stop(d) → undefined`
  - `console_dispatch_wait(d) → undefined`
  - `console_dispatch_destroy(d) → undefined`
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/console_dispatch.h`, `console_dispatch.c`

### console_events
- **Namespace:** None (internal; single-producer/single-consumer ring buffer for console events)
- **Structures (C API only):**
  - `ConsoleEventQueue` — Ring buffer (capacity 256).
- **Functions:**
  - `console_eventq_init(q)`, `console_eventq_destroy(q)`, `console_eventq_push(q, event) → bool`, `console_eventq_pop(q, out) → bool`, `console_eventq_pop_wait(q, out, timeout_ms) → bool`
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/console_events.h`, `console_events.c`

### console_input
- **Namespace:** None (internal; VT escape sequence decoder)
- **Structures (C API only):**
  - `ConsoleKeyEvent` — Key name, raw bytes, modifiers (ctrl, alt, shift, meta).
  - `ConsoleMouseEvent` — x, y (1-based), button, action, modifiers.
  - `ConsoleEvent` — Union of key/mouse events.
  - `ConsoleInputState` — Decoder state machine (32-byte buffer, pending ESC marker).
- **Functions:**
  - `console_input_feed(state, byte, out) → ConsoleInputResult` — Feed one byte; return PENDING or DONE_KEY or DONE_MOUSE or DROP.
  - `console_input_flush(state, out) → ConsoleInputResult` — Commit pending ESC as Escape key event.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/console_input.h`, `console_input.c`

### console_lineedit
- **Namespace:** None (internal; cooked-mode line editor)
- **Bare-metal blockers:** POSIX termios or Windows console API
- **Functions (C API only):**
  - `console_lineedit_read(prompt, password, out, out_len) → int` — Read one line with editing. Returns 1 on success (Enter), 0 on Ctrl-D/EOF, -1 on error.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/console_lineedit.h`, `console_lineedit.c`

### console_term
- **Namespace:** None (internal; cross-platform terminal control)
- **Bare-metal blockers:** POSIX termios + ANSI sequences or Windows console API
- **Functions (C API only):**
  - Lifecycle: `console_term_init()`, `console_term_shutdown()`, `console_term_detach()`, `console_term_attach() → bool`, `console_term_exists() → bool`
  - Window control: `console_term_hide()`, `console_term_show()`, `console_term_set_title(title, len)`
  - Output: `console_term_write(data, len) → bool`, `console_term_flush()`
  - Mode: `console_term_set_raw(on) → bool`, `console_term_set_echo(on) → bool`, `console_term_set_cursor_visible(on)`, `console_term_set_alternate_screen(on)`, `console_term_set_mouse(on)`, `console_term_is_tty() → bool`
  - Size: `console_term_size(cols, rows)`
  - Resize: `console_term_resize_pending() → bool`, `console_term_resize_clear()`
  - Input: `console_term_input_fd() / console_term_input_handle()`, `console_term_read_input(buf, cap, timeout_ms) → int`
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/console_term.h`, `console_term.c`

### app
- **Namespace:** `app`
- **Bare-metal blockers:** None (process state management)
- **Functions:**
  - `app.quit([code]) → null` — Request graceful shutdown with optional exit code.
  - `app.exit([code]) → never` — Hard _exit() after running quit hooks.
  - `app.isQuitting() → bool` — True if quit() has been called.
  - `app.holds() → number` — Current lifeline count (internal reference count for shutdown delay).
  - `app.exitCode([code]) → number` — Get or set the process exit code.
  - `app.onQuit(fn) → undefined` — Register callback to run on quit. TBD: confirm support (docs mention it as planned but may not be implemented).
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/app.h`, `app.c`

### array
- **Namespace:** `array` (also prototype on all array instances)
- **Functions:**
  - `array.length(a) → number` — Array length.
  - `array.push(a, v) → bool` — Append element; return true.
  - `array.push(a, index, v) → bool` — Insert element at index.
  - `array.pop(a) → value | null` — Remove and return last element.
  - `array.splice(a, start, len?) → array` — Remove len elements starting at start; return array of removed.
  - `array.remove(a, index) → value | null` — Remove element at index.
  - `array.copy(a) → array` — Shallow copy.
  - `array.map(a, f) → array` — Map function over elements. `f` signature: `fn(elem, index, array) → value`.
  - `array.filter(a, f) → array` — Filter elements via predicate function.
  - `array.reduce(a, f, init?) → value` — Reduce with accumulator. `f` signature: `fn(accum, elem, index, array) → value`.
  - `array.forEach(a, f) → undefined` — Iterate without collecting results.
  - `array.find(a, f) → value | null` — Return first element matching predicate.
  - `array.findIndex(a, f) → number` — Return index of first match.
  - `array.indexOf(a, elem, from?) → number` — Index of elem (linear search).
  - `array.lastIndexOf(a, elem, from?) → number`
  - `array.includes(a, elem) → bool`
  - `array.join(a, sep?) → string` — Join elements with separator.
  - `array.slice(a, start?, end?) → array` — Slice elements.
  - `array.concat(a, ...others) → array` — Concatenate arrays.
  - `array.reverse(a) → array` — Reverse in-place. Returns mutated array.
  - `array.sort(a, cmp?) → array` — Sort in-place. Optional comparator: `cmp(a, b) → number` (negative/zero/positive).
  - `array.unique(a) → array` — Return unique elements (preserves order, uses === for equality). TBD: confirm.
  - `array.flat(a, depth?) → array` — Flatten nested arrays.
  - `array.some(a, f) → bool` — True if any element matches predicate.
  - `array.every(a, f) → bool` — True if all elements match predicate.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/array.h`, `array.c`

### csv
- **Namespace:** `csv`
- **Bare-metal blockers:** None (pure parsing)
- **Functions:**
  - `csv.parse(str, delim?, header?) → array` — Parse CSV. If header=true (default), return array of objects; else array of arrays. delim defaults to ",".
  - `csv.stringify(data, delim?, headers?) → string` — Stringify array of objects (or arrays). Returns RFC 4180 CSV with CRLF line endings.
- **Return convention:** On parse error, throws via cando_vm_error.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/csv.h`, `csv.c`

### eval
- **Namespace:** Global function `eval()`
- **Functions:**
  - `eval(source) → value` — Compile and execute source string in current scope. Returns the value of the last expression.
- **Error convention:** Syntax errors, runtime errors throw.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/eval.h`, `eval.c`

### gc
- **Namespace:** `gc`
- **Functions:**
  - `gc.collect() → number` — Sweep unreachable objects. Returns number of objects freed.
  - `gc.count() → number` — Count currently tracked live objects.
  - `gc.threshold([n]) → number` — Get or set the auto-collect threshold. n=0 disables auto-collection.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/gc.h`, `gc.c`

### include
- **Namespace:** Global function `include()`
- **Bare-metal blockers:** POSIX file I/O, dlopen for binary modules
- **Functions:**
  - `include(path) → value` — Load a .cdo script or binary module (.so/.dylib/.dll). Scripts are compiled in eval mode and executed; return value is the value of the last expression (or explicit RETURN). Binary modules call `cando_module_init(CandoVM *vm) → CandoValue` symbol and return its result. Paths are cached (by resolved absolute path); subsequent calls return the cached value.
- **Error convention:** File not found, compile error, module load failure throw.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/include.h`, `include.c`

### jit
- **Namespace:** `jit`
- **Functions:**
  - `jit.on() → undefined` — Enable profiling counters.
  - `jit.off() → undefined` — Disable profiling counters.
  - `jit.toggle() → string` — Flip state, return "on" or "off".
  - `jit.status() → string` — Return "on" or "off".
  - `jit.isAvailable() → bool` — True if JIT is compiled in (always true in Phase 2+).
  - `jit.stats() → {backedges, func_entries, iter_next}` — Return profiling counter values.
  - `jit.reset() → undefined` — Zero all counters.
- **Note:** Phase 2 plumbing only; no machine code generation yet.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/jit.h`, `jit.c`

### json
- **Namespace:** `json`
- **Bare-metal blockers:** None (pure parsing/serialization)
- **Functions:**
  - `json.parse(str) → value | null` — Parse JSON string. Returns null on parse error (does not throw). Maps JSON types: object→object, array→array, string→string, number→number, true/false→bool, null→null.
  - `json.stringify(val, indent?) → string` — Serialize value to JSON. Optional indent (number of spaces, default compact). Functions/natives serialized as null. Returns empty string on allocation failure.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/json.h`, `json.c`

### libutil
- **Namespace:** None (internal utility library for native authors)
- **Functions (C API only; inlined and non-inlined):**
  - **Argument extractors (inlined):**
    - `libutil_arg_cstr(v) → const char *` — Return C string or NULL.
    - `libutil_arg_str(v) → CandoString *` — Return string object or NULL.
    - `libutil_arg_num(v, def) → f64` — Return number or default.
    - `libutil_arg_cstr_at(args, argc, idx) → const char *`
    - `libutil_arg_str_at(args, argc, idx) → CandoString *`
    - `libutil_arg_num_at(args, argc, idx, def) → f64`
  - **Validators (non-inlined; raise VM error on failure):**
    - `libutil_require_cstr_at(vm, args, argc, idx, fn_name) → const char *`
    - `libutil_require_str_at(vm, args, argc, idx, fn_name) → CandoString *`
    - `libutil_require_num_at(vm, args, argc, idx, fn_name, out) → bool`
    - `libutil_require_object_at(vm, args, argc, idx, fn_name, out_obj) → bool`
  - **String push helpers:**
    - `libutil_push_str(vm, data, len) → undefined` — Create string from bytes and push.
    - `libutil_push_cstr(vm, str) → undefined` — Create string from C string and push.
  - **Method registration:**
    - `libutil_set_method(vm, obj, name, fn) → undefined` — Register one method.
    - `libutil_register_methods(vm, obj, entries, count) → undefined` — Bulk register from static table.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/libutil.h`, `libutil.c`

### math
- **Namespace:** `math`
- **Bare-metal blockers:** C math library (libm: sin/cos/tan/exp/log/sqrt/etc.)
- **Functions:**
  - Basic: `math.abs(x)`, `math.sign(x)`, `math.min(...)`, `math.max(...)`, `math.clamp(x, min, max)`
  - Rounding: `math.floor(x)`, `math.ceil(x)`, `math.round(x)`, `math.trunc(x)`
  - Trig: `math.sin(x)`, `math.cos(x)`, `math.tan(x)`, `math.asin(x)`, `math.acos(x)`, `math.atan(x)`, `math.atan2(y, x)`
  - Hyperbolic: `math.sinh(x)`, `math.cosh(x)`, `math.tanh(x)`
  - Exponential/Log: `math.exp(x)`, `math.log(x)`, `math.log10(x)`, `math.log2(x)`, `math.pow(x, y)`, `math.sqrt(x)`, `math.cbrt(x)`
  - Special: `math.hypot(x, y)`, `math.fmod(x, y)`, `math.remainder(x, y)`
  - Random: `math.random() → number` (0.0 to 1.0), `math.randomSeed(seed)`
  - Constants: `math.PI`, `math.E`, `math.LN2`, `math.LN10`, `math.LOG2E`, `math.LOG10E`, `math.SQRT1_2`, `math.SQRT2`
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/math.h`, `math.c` (header is incomplete; read .c for full list)

### meta
- **Namespace:** `_meta` (global writable object)
- **Functions:**
  - **Registration (C API only):**
    - `cando_lib_meta_register(vm) → undefined` — Create empty `_meta` global. Idempotent.
    - `cando_lib_meta_table(vm, name) → CdoObject *` — Get or create meta table for a type.
    - `cando_lib_meta_set(vm, name, table) → undefined` — Register a pre-built meta table.
    - `cando_lib_meta_attach(vm, instance, name) → undefined` — Set instance.__index = _meta.<name>.
    - `cando_lib_meta_define(vm, tbl, name, fn) → undefined` — Register method only if not already present.
    - `cando_lib_meta_alias(dst, dst_name, src, src_name) → undefined` — Copy field from src to dst under new name.
  - **Script-side:**
    - `_meta.<type_name>` — Object serving as prototype for instances of that type. User-writable; methods added here are available on all instances.
- **Common meta table names:** `tcp_socket`, `tcp_server`, `tls_socket`, `tls_server`, `http_response`, `http_request`, `http_server`, `https_server`, `stream`, `string`, `array`, `object`
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/meta.h`, `meta.c`

### object
- **Namespace:** `object` (also prototype on all object instances)
- **Functions:**
  - `object.lock(o) → undefined` — Acquire exclusive write lock on object.
  - `object.locked(o) → bool` — True if object is write-locked by any thread.
  - `object.unlock(o) → undefined` — Release exclusive write lock.
  - `object.copy(o) → object` — Shallow copy.
  - `object.assign(o, ...sources) → object` — Merge sources into o (mutates o). Returns o.
  - `object.apply(o, ...sources) → object` — Merge o + sources into new object. Returns new object (non-mutating).
  - `object.get(o, key) → value` — Raw field get (bypasses __index).
  - `object.set(o, key, v) → bool` — Raw field set (bypasses __newindex). Returns true on success.
  - `object.setPrototype(o, proto) → undefined` — Set o.__index = proto.
  - `object.getPrototype(o) → object | null` — Get o.__index.
  - `object.keys(o) → array` — Array of field names (insertion order).
  - `object.values(o) → array` — Array of field values (insertion order).
  - `object.entries(o) → array` — Array of [key, value] pairs.
  - `object.fromEntries(arr) → object` — Build object from [[key, value], ...] array.
  - `object.has(o, key) → bool` — True if field exists.
  - `object.freeze(o) → object` — Make object immutable. Returns o.
  - `object.isFrozen(o) → bool` — True if frozen.
  - `object.seal(o) → object` — Prevent add/delete, allow modify. Returns o.
  - `object.isSealed(o) → bool` — True if sealed.
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/object.h`, `object.c`

### string
- **Namespace:** `string` (also prototype on all string instances)
- **Functions:**
  - `string.length(s) → number` — String length in bytes (not codepoints). TBD: confirm.
  - `string.charAt(s, index) → string` — Character at index.
  - `string.charCode(s, index) → number` — UTF-8 codepoint at index. TBD: confirm.
  - `string.fromCharCode(...codes) → string` — Build string from codepoints.
  - `string.codePointAt(s, index) → number` — Unicode codepoint.
  - `string.indexOf(s, substr, from?) → number` — Index of substring (-1 if not found).
  - `string.lastIndexOf(s, substr, from?) → number`
  - `string.includes(s, substr) → bool` — True if substring present.
  - `string.startsWith(s, prefix) → bool`
  - `string.endsWith(s, suffix) → bool`
  - `string.slice(s, start, end?) → string` — Extract substring.
  - `string.substring(s, start, end?) → string` — Extract substring (bounds-clamped).
  - `string.substr(s, start, len?) → string` — Extract len chars starting at start.
  - `string.toUpperCase(s) → string` — Convert to uppercase (UTF-8 aware).
  - `string.toLowerCase(s) → string` — Convert to lowercase.
  - `string.trim(s) → string` — Remove leading/trailing whitespace.
  - `string.trimStart(s) → string` — Remove leading whitespace.
  - `string.trimEnd(s) → string` — Remove trailing whitespace.
  - `string.split(s, sep?, limit?) → array` — Split into array of substrings.
  - `string.repeat(s, count) → string` — Repeat string count times.
  - `string.replace(s, pattern, replacement) → string` — Replace first or all (depending on pattern type). TBD: confirm regex vs string.
  - `string.replaceAll(s, pattern, replacement) → string`
  - `string.padStart(s, len, pad?) → string` — Pad start to len with pad string (default " ").
  - `string.padEnd(s, len, pad?) → string`
  - `string.reverse(s) → string` — Reverse string.
  - `string.match(s, pattern) → array | null` — Match regex. TBD: confirm regex syntax and return value.
  - `string.format(fmt, ...args) → string` — Printf-style formatting. TBD: read .c for format specifiers.
  - `string.fromBytes(bytes, encoding?) → string` — Decode bytes. encoding defaults to "utf8".
  - `string.toBytes(s, encoding?) → string` — Encode to bytes. encoding defaults to "utf8".
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/string.h`, `string.c`

### yaml
- **Namespace:** `yaml`
- **Bare-metal blockers:** None (self-contained YAML parser/emitter)
- **Functions:**
  - `yaml.parse(str) → value | null` — Parse YAML document. Returns null on parse error (does not throw). Maps block/flow mappings to objects, sequences to arrays, scalars to strings/numbers/bool/null with heuristic type inference ("true"/"yes"→true, "123"→number, etc.).
  - `yaml.stringify(val, indent?) → string` — Serialize value to YAML 1.2. Indent defaults to 2 (range 1-16).
- **Citation:** `/home/user/CanBoot/vendor/cando/source/lib/yaml.h`, `yaml.c`

---

## Summary of key patterns for CanBoot rewrite

1. **Native function signature:** All native functions follow `int fn(CandoVM *vm, int argc, CandoValue *args)`, returning the count of values pushed onto the stack or -1 on error (via `cando_vm_error`).

2. **CLASS instances and method dispatch:** Every built-in type (Socket, Stream, HTTP Response, Server) is a CdoObject with:
   - A hidden field (e.g., `__socket_id`) holding a handle to C state (usually an index into a static slot pool).
   - A meta table (e.g., `_meta.tcp_socket`) serving as the prototype (`__index`).
   - Registration via `libutil_set_method` or `libutil_register_methods` for module globals; `cando_lib_meta_define` for type methods.

3. **Slot pool pattern:** Socket, Stream, and other resource types use a fixed-size slot pool (e.g., `SOCKET_MAX_INSTANCES = 256`) to avoid heap fragmentation and support handle recycling. The slot pool is shared across libraries where appropriate (socket pool is shared with secure_socket).

4. **Thread-based async:** Server listeners, HTTP servers, and console input dispatch spawn background worker threads that call user callbacks inside child VMs. Child VMs are initialized via `cando_vm_init_child(parent)` so they inherit handles and globals.

5. **Stream adapter pattern:** The `stream` library defines a vtable (read/write/flush/end/destroy/seek/tell) that file, socket, HTTP, and process libraries implement. This allows a single Stream object to wrap any I/O source.

6. **Metadata and extensibility:** The global `_meta` object holds prototypes for all built-in types. Users can add methods to `_meta.tcp_socket`, etc., and those methods are available on all instances via `__index` chaining. This is the primary extension mechanism (no event emitters or hooks).

