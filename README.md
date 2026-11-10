# Vortex

A static-file HTTP/1.1 server for Linux, written from scratch in C++17 with no dependencies beyond the standard library and POSIX. It uses one `epoll` event-loop thread, a pool of worker threads, and `sendfile()` for zero-copy file transfer.

The goal was to build the core of a real web server (non-blocking I/O, backpressure, keep-alive, safe file serving) and to test it properly rather than just claim it works.

## Features

- **HTTP/1.1 static file serving**: `GET` and `HEAD`, keep-alive, pipelined requests, HTTP/1.0 clients, MIME types, a `Date` header and correct error codes (400, 403, 404, 405, 413, 414, 431, 505).
- **Event loop plus worker pool**: one thread waits in `epoll_wait()` and hands ready connections to a fixed pool of workers.
- **Non-blocking connection state machine**: partial reads and partial writes are handled explicitly. When a client's socket buffer is full, the connection waits for `EPOLLOUT` and the worker moves on. Workers never sleep or spin on a socket.
- **Fairness**: a worker sends at most 1 MiB per turn before re-queueing the connection, so one large download can't monopolise a thread.
- **Zero-copy transfer** with `sendfile()`, plus a `--no-sendfile` mode that copies through user space, for comparison.
- **Safe file serving**: blocks `..` (including percent-encoded forms), symlinks that point outside the document root, and dotfiles like `.git` and `.env`. Headers are capped at 8 KiB.
- **Robustness**: graceful shutdown on `SIGINT`/`SIGTERM`, `SIGPIPE`-safe writes, an `EMFILE` reserve descriptor so running out of file descriptors doesn't stall the accept loop, and every system-call result checked.
- **Tests**: unit and end-to-end suites run through CTest, and are clean under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.

## Architecture

```text
                    ┌──────────────────────────────┐
  clients ────────▶ │  main thread: epoll_wait()   │
                    │  • accept4() new clients     │
                    │  • eventfd wake-up for stop()│
                    └──────────────┬───────────────┘
                                   │ ready Connection*
                                   ▼
                    ┌──────────────────────────────┐
                    │  ThreadPool task queue       │
                    └──────────────┬───────────────┘
                                   ▼
                    ┌──────────────────────────────┐
                    │  worker: Connection::on_ready│
                    │  read → parse → resolve path │
                    │  → send head → sendfile()    │
                    └──────────────┬───────────────┘
                                   │ returns WantRead / WantWrite / Close
                                   ▼
                    re-arm in epoll (EPOLLIN or EPOLLOUT), or close
```

### Design decisions

- **`EPOLLONESHOT` gives each connection a single owner.** Once epoll reports a socket, it is disarmed until the worker handling it re-arms it. Two workers can never touch the same connection at once, so `Connection` needs no locks.
- **Level-triggered client sockets.** Re-arming with `EPOLL_CTL_MOD` makes the kernel check readiness again, so any data that arrived while a worker was busy is reported straight away. Edge-triggered mode would add the risk of missed wake-ups and no benefit here. The listening socket is also level-triggered, so an `accept()` that stops early can't leave clients stuck in the backlog.
- **Backpressure instead of sleeping.** If `send()` or `sendfile()` returns `EAGAIN`, the connection is re-armed for `EPOLLOUT` and the worker is freed. An earlier version slept and retried in that case; with a handful of slow clients that froze the whole server (see [Benchmarks](#benchmarks)).
- **Ownership and lifetime.** epoll's `data.ptr` points at the `Connection`. The server keeps every connection in a map behind a mutex, but only uses it to create connections, close them, and clean up at shutdown, never on the request path.
- **Path resolution checks twice.** URL segments are checked first (rejecting `..` and dotfiles), then `realpath()` confirms the final file really is inside the document root. The second check is what catches symlinks.

### Source layout

```text
include/ src/
├── concurrency/ThreadPool    fixed-size worker pool with a condition-variable task queue
├── http/Request              request-line and header parser (validation, percent-decoding, keep-alive rules)
├── http/Response             status lines, headers, error pages
├── http/StaticFiles          URL path → file on disk, traversal protection, MIME types
├── network/EpollServer       listening socket, epoll loop, accept, re-arming, shutdown
├── network/Connection        per-connection read/parse/write state machine
└── utils/Logger, UniqueFd    thread-safe logger, RAII file-descriptor wrapper
tests/                        unit tests and end-to-end tests over real sockets
scripts/benchmark.sh          ApacheBench wrapper
public/                       demo site served by default
```

## Build and run

Requires Linux (WSL2 works), a C++17 compiler and CMake 3.14+.

```bash
git clone https://github.com/anurag67-droid/Vortex.git
cd Vortex
cmake -S . -B build
cmake --build build -j
./build/vortex_server          # serves ./public on http://localhost:8080
```

| Option | Default | |
|---|---|---|
| `-p, --port N` | `8080` | `0` picks any free port |
| `-r, --root DIR` | `public` | document root |
| `-t, --threads N` | CPU cores | worker threads |
| `--no-sendfile` | off | copy files through user space instead (benchmark baseline) |
| `-v, --verbose` | off | log every request |

Stop the server with `Ctrl+C`: it finishes in-flight work and closes connections cleanly.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

| Suite | What it checks |
|---|---|
| `parser` | request-line and header validation, percent-decoding, keep-alive rules, body detection |
| `static_files` | traversal (plain, nested, via symlinks), dotfiles, directory indexes, MIME types |
| `threadpool` | 1,000 tasks across workers; the destructor waits for running tasks |
| `server` | starts a real server and talks to it over TCP: keep-alive, pipelining, `HEAD`, split packets, traversal attempts, error codes, a 16 MiB file arriving intact, 200 concurrent clients, slow clients not blocking others, shutdown with connections open. The suite runs in both `sendfile` and copy mode. |

Sanitizer builds:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DVORTEX_SANITIZE=address,undefined
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DVORTEX_SANITIZE=thread
cmake --build build-tsan -j && setarch -R ctest --test-dir build-tsan --output-on-failure
```

(`setarch -R` turns off address randomisation for the test run. ThreadSanitizer refuses to start under the high ASLR entropy that recent kernels use.)

## Benchmarks

Measured with ApacheBench over loopback on a 16-core AMD Ryzen 7 7840HS (WSL2, kernel 6.18), Release build, default thread count. Each number is the median of 3 runs after a warm-up, and every run had 0 failed requests. "Original" is this repository's code before the rewrite.

| Server | File | Connections | Concurrency | Requests/s |
|---|---|---|---|---:|
| original | `index.html` (3.7 KB) | new per request | 1,000 | 10,404 |
| Vortex | `index.html` (3.7 KB) | new per request | 1,000 | 10,889 |
| Vortex | `index.html` (3.7 KB) | keep-alive | 1,000 | **46,607** |
| Vortex `--no-sendfile` | `index.html` (3.7 KB) | keep-alive | 1,000 | 44,984 |
| Vortex | 1 MB file | keep-alive | 100 | **2,816** (~2.8 GB/s) |
| Vortex `--no-sendfile` | 1 MB file | keep-alive | 100 | 2,601 (~2.6 GB/s) |

What the numbers show:

- **Keep-alive is the big win: about 4.3× more requests per second.** Without it, every request pays for a TCP handshake and teardown, and that cost dominates for small files.
- **`sendfile()` helps, but modestly**: about 8% for 1 MB files and within noise for small ones. Over loopback, an extra memory copy is cheap compared with the TCP stack itself. The gap would widen for larger files and under CPU pressure.
- **ab is likely the bottleneck for small files.** It runs on a single thread, so these numbers are a lower bound on what the server can handle.

The biggest difference from the rewrite isn't throughput, though; it's behaviour under slow clients. With 8 clients downloading a 64 MB file at 1 KB/s:

| | Request for `/` while the slow downloads run | `GET /../../../../../../../../etc/passwd` |
|---|---|---|
| original | **no response; timed out after 5 s** (every worker stuck retrying the slow clients) | **200, contents of `/etc/passwd`** |
| Vortex | 200 in 0.5 ms | 403 |

To reproduce:

```bash
./build/vortex_server &                                   # add --no-sendfile for the baseline
scripts/benchmark.sh http://127.0.0.1:8080/ 200000 1000 -k
```

On WSL, keep the files you serve on the Linux filesystem (for example under `~`), not under `/mnt/c`. Reading Windows files through WSL is roughly 10× slower and would dominate the results.

## Limitations

- **No idle timeouts.** An idle keep-alive connection holds a file descriptor until the client closes it, so many idle clients (a "slowloris" attack) can exhaust descriptors. Adding a timer wheel or `timerfd` is the next step.
- Serves static files only: no request bodies, TLS, HTTP/2, range requests, chunked encoding, compression or caching headers (`ETag`/`Last-Modified`).
- A URL naming a directory without a trailing slash serves its `index.html` directly instead of redirecting.
- Linux-only (`epoll`, `sendfile`, `accept4`, `eventfd`).

## Contributors

Built by [anurag67-droid](https://github.com/anurag67-droid) and [ArjunTomar-G](https://github.com/ArjunTomar-G).
