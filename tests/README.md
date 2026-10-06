# ThreadPool regression tests

The standalone CMake test build runs a portable public-API suite on Linux,
Windows, and macOS. Linux additionally runs the construction, resizing, and
shutdown failure-injection suites. No external test framework is required.

Run from the ThreadPool repository root:

```sh
cmake -S tests -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_CXX_STANDARD=11 -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error --parallel 2
```

Choose `clang++` instead of `g++` for Clang, or use `xcrun --find clang++` for
Apple Clang. Use separate build directories when changing the compiler or
language standard. MSVC has no C++11 mode: use `CMAKE_CXX_STANDARD=14` for its
minimum mode, including clang-cl with the MSVC standard library.

On Windows, this PowerShell helper selects an x64 Visual Studio environment,
installs a missing requested MSVC component, and sets `CXX` to the exact compiler:

```powershell
./tests/setup-windows.ps1 -Toolset v143 -Compiler MSVC
cmake -S tests -B build -G Ninja -DCMAKE_CXX_STANDARD=14 -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error --parallel 2
```

The helper accepts `v140`, `v141`, `v142`, `v143`, and `v145`; select `Clang`
instead of `MSVC` to use the bundled Visual Studio clang-cl. It expects Visual
Studio 2022 for v140-v143 or Visual Studio 2026 for v145. C++23 on native MSVC
requires `/std:c++23preview`; configuration fails if the switch is unsupported.
The v140 environment selects Windows SDK 10.0.19041.0, installing it if needed;
newer SDK headers use compiler intrinsics that toolset does not support.

## Continuous integration

GitHub Actions runs on pushes, pull requests, and manual dispatch. A Linux
GCC 12/C++11 Release job runs all 45 cases first. Every other job depends on its
success; a failed gate skips the compiler matrix. Each compiler job builds and
tests its language modes in separate directories, using Release throughout:

| Platform | Compiler | Language modes |
| --- | --- | --- |
| Ubuntu 24.04 x64 | GCC 12, 13, 14; Clang 16, 17, 18 | C++11, 17, 20, 23 |
| Windows Server 2022 x64 | MSVC v140 | C++14 minimum |
| Windows Server 2022 x64 | MSVC v141 | C++14 minimum, 17 |
| Windows Server 2022 x64 | MSVC v142 | C++14 minimum, 17, 20 |
| Windows Server 2022 x64 | MSVC v143 | C++14 minimum, 17, 20, 23 preview |
| Windows Server 2025 x64 / VS 2026 | MSVC v145 | C++14 minimum, 17, 20, 23 preview |
| Both Windows images | Bundled Visual Studio clang-cl | C++14 minimum, 17, 20, 23 |
| macOS 15 ARM64 / Xcode 16.4 | Apple Clang | C++11, 17, 20, 23 |

The matrix does not repeat GCC 12/C++11 after the gate. Windows and macOS run
the 14 portable cases; all Linux compiler jobs also run the 31 interposition
cases. Missing toolchains and unsupported requested modes fail the job rather
than reducing coverage. Compiler versions and language modes appear in logs.
Clang 17 uses the installed GCC 13 standard library because GCC 14's C++23
tuple constraints are incompatible with that compiler.
An internal result trait works around VS 2015's variadic alias substitution;
the public `return_type` alias and enqueue signatures are unchanged.

## Portable behavior suite

`portable.cpp` exercises the unchanged ThreadPool public API. Its cases are:

- `results`: callable arguments, reference binding, returned values, task
  exceptions, and continued execution after an exception.
- `zero`: construction and resizing with zero request one usable worker.
- `resize`, `busy-resize`: growth, shrinkage, and busy-worker slot reuse.
- `worker-resize`, `concurrent`: resizing from tasks or competing callers,
  with exactly-once work completion.
- `drain`: destruction completes each queued task and preserves its result.
- `queue-full`: a full queue returns a future with `would_block`, including
  normalization of a zero queue limit.
- `enqueue-block`, `enqueue-block-growth`: a blocked producer resumes when a
  worker drains the queue or the caller increases its capacity.
- `wait-empty`, `wait-idle`: distinguish an empty queue from completed tasks.
- `shutdown-active`, `shutdown-retired`: destruction waits for gated C++ TLS
  cleanup of active and previously retired workers.

Checks stay enabled under `NDEBUG`. Condition-variable gates and ten-second
deadlines coordinate tasks and TLS cleanup. Bounded future waits check that
operations remain blocked while their required resource is deliberately held.
CTest runs every case in its own process with a 90-second timeout. Passing one
case name directly to an executable selects it; no argument runs all cases.

## Linux failure-injection suites

These standalone Linux tests use `pthread_create` interposition and
`dlsym(RTLD_NEXT, ...)` to inject `EAGAIN` without exhausting system resources.
No production test hooks or external test framework are required.

`worker_failure.cpp` covers construction, zero-size normalization, failed growth,
slot reuse during downsizing, oversized reserve requests, and task draining. Its
`allocation` case overrides `operator new` on the resizing caller to fail reserve,
worker-record, and thread-state allocation, including after partial startup.
Allocation failures remain active through recovery; test bookkeeping inside the
`pthread_create` hook is excluded. It verifies recovery does not allocate again.

`worker_shutdown.cpp` additionally interposes `pthread_join` and `pthread_detach`
to check that every thread, including the joiner, is joined exactly once and none
is detached. It records the joining caller, distinguishing the helper from pool
workers and external callers. Gated C++ and native pthread TLS destructors
separate leaving the task loop from complete thread exit. Pool-created threads
are tracked, while async test-control threads are excluded.

For additional local diagnostics, compile the Linux suites with sanitizers:

```sh
set -e
for test in worker_failure worker_shutdown; do
    g++ -std=c++23 -O1 -g -DNDEBUG -fsanitize=address,undefined \
        -fno-omit-frame-pointer -pthread -I. tests/$test.cpp -ldl \
        -o /tmp/threadpool-$test-sanitized
    timeout 90s /tmp/threadpool-$test-sanitized
done
```

If a sandbox prevents LeakSanitizer from inspecting threads at process exit,
prefix the sanitized run with `ASAN_OPTIONS=detect_leaks=0`. AddressSanitizer
and UndefinedBehaviorSanitizer remain enabled.

Check concurrent resizing with ThreadSanitizer and worker lifetimes with Valgrind:

```sh
g++ -std=c++11 -O1 -g -fsanitize=thread -fno-omit-frame-pointer -pthread -I. \
    tests/worker_shutdown.cpp -ldl -o /tmp/threadpool-worker_shutdown-tsan
timeout 120s /tmp/threadpool-worker_shutdown-tsan

for test in worker_failure worker_shutdown; do
    timeout 120s valgrind --quiet --error-exitcode=1 --leak-check=full \
        --soname-synonyms=somalloc=nouserintercepts \
        build/$test
done
```

The Valgrind option preserves the test executable's allocation override while
still intercepting allocations in system libraries. Without it, Memcheck
replaces `operator new` and the `allocation` case cannot inject its failures.

Checks remain enabled with `NDEBUG`. Synchronization uses gates and condition
variables with five-second (`worker_failure`) or ten-second (`worker_shutdown`)
deadlines. Outer timeouts also bound destruction. An argument selects one case;
without arguments each executable runs all its cases.

The failure executable accepts:

- `construct`, `construct-reserve`: failures before and after worker startup;
  constructor cleanup must finish before propagating the original exception.
- `zero`: one usable worker for a zero-size request and complete task draining.
- `first`, `partial`: failed growth, retry, task execution, and further resizing.
  A sole partially started worker becomes the joiner and lives until destruction.
- `downsizing`: busy workers are reused during growth; a failed growth leaves
  them retiring against the previous target.
- `reserve`, `allocation`: allocation failures precede unsafe thread ownership
  and preserve a usable pool, including allocation-free worker promotion.
- `tasks`: 3,000 queued tasks during repeated resizing, each completed once.

The shutdown executable accepts:

- `shutdown-active`, `shutdown-retired`, `shutdown-native-active`,
  `shutdown-native-retired`: destruction waits for C++ or native TLS cleanup of
  active and previously retired workers.
- `independent`: a lower-index worker retires while the highest-index worker
  remains busy, preserving the historical livelock fix.
- `worker-resize`: worker tasks and TLS destructors resize without joining.
- `resize-worker`, `resize-concurrent`, `resize-native`: enqueue, resizing, slot
  reuse, and cleanup-origin work submission progress while the joiner is blocked
  in another worker's TLS cleanup.
- `construct`: constructor failure waits for complete worker exit.
- `lazy`: construction, growth, unchanged targets, and destruction need no helper.
- `shrink-failure`, `shrink-worker-failure`: helper startup failure leaves the
  original parallelism intact; a retry starts exactly one helper.
- `failed-first`, `failed-growth`, `failed-several`, `failed-existing`: growth
  failure before startup, after one or several starts, or with an existing
  helper. All later creation attempts also fail until recovery returns, and
  recovery must make no further attempt. Surplus workers retire against the old
  target; subsequent tasks and resizing succeed.
- `autonomous`: thirty worker-only shrink/grow cycles reclaim retirees without
  external resize/collection calls and retain only one helper.
- `shutdown-joiner`, `shutdown-promoted`, `shutdown-immediate`: shutdown joins an
  idle or promoted helper through its native TLS cleanup, drains queued work,
  and handles destruction immediately after promotion.
- `concurrent`: competing external resizers use one helper and complete all
  queued tasks exactly once.

To compare revisions, compile with `-I` pointing to a directory containing that
revision's `ThreadPool.h`, keeping the test source unchanged. `autonomous` fails
against `c571908` because worker-origin resizing never reclaims retired records.
The original `first` reproducer asserts or can crash before the failed-growth
fix; the shutdown gates detect early destructor return with the former detaching
implementation.
