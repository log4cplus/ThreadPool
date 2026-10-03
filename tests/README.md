# ThreadPool construction, resizing, and shutdown regression tests

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

Run from the ThreadPool repository root:

```sh
set -e
for compiler in g++ clang++; do
    for test in worker_failure worker_shutdown; do
        "$compiler" -std=c++11 -O0 -g -Wall -Wextra -Wpedantic -pthread -I. \
            tests/$test.cpp -ldl -o /tmp/threadpool-$test-debug
        timeout 90s /tmp/threadpool-$test-debug

        "$compiler" -std=c++11 -O2 -DNDEBUG -g -Wall -Wextra -Wpedantic -pthread -I. \
            tests/$test.cpp -ldl -o /tmp/threadpool-$test-release
        timeout 90s /tmp/threadpool-$test-release
    done
done

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
        /tmp/threadpool-$test-debug
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
