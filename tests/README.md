# ThreadPool construction, resizing, and shutdown regression tests

`worker_failure.cpp` is a standalone Linux test using `pthread_create`
interposition and `dlsym(RTLD_NEXT, ...)`. It injects `EAGAIN` without exhausting
system resources and tracks when partially started workers retire. No production
test hooks or external test framework are required.

`worker_shutdown.cpp` additionally interposes `pthread_join` and `pthread_detach`
to check that every worker is joined exactly once and none is detached. Gated
thread-local destructors distinguish leaving the task loop from complete thread
exit. Pool worker creations are tracked, while async test-control threads are not.

Run these commands from the ThreadPool repository root, using GCC or Clang:

```sh
set -e
for test in worker_failure worker_shutdown; do
    g++ -std=c++11 -O0 -g -Wall -Wextra -Wpedantic -pthread -I. \
        tests/$test.cpp -ldl -o /tmp/threadpool-$test-debug
    timeout 90s /tmp/threadpool-$test-debug

    g++ -std=c++11 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -pthread -I. \
        tests/$test.cpp -ldl -o /tmp/threadpool-$test-release
    timeout 90s /tmp/threadpool-$test-release

    g++ -std=c++23 -O1 -g -DNDEBUG -fsanitize=address,undefined \
        -fno-omit-frame-pointer -pthread -I. tests/$test.cpp -ldl \
        -o /tmp/threadpool-$test-sanitized
    timeout 90s /tmp/threadpool-$test-sanitized
done
```

Repeat the C++11 commands with `clang++` in place of `g++`.

If a sandbox prevents LeakSanitizer from inspecting threads at process exit,
prefix the sanitized run with `ASAN_OPTIONS=detect_leaks=0`. AddressSanitizer
and UndefinedBehaviorSanitizer remain enabled.

Also check independent retirement and shutdown with Valgrind:

```sh
timeout 120s valgrind --quiet --error-exitcode=1 --leak-check=full \
    /tmp/threadpool-worker_shutdown-debug
```

Construction tests inject failure on the first, second, and fourth worker. They
check that the original exception propagates, all started workers retire, and
another pool can be constructed and used afterward. An oversized-construction
case verifies that reserve fails before any thread creation is attempted.
The zero-size case checks that construction starts one worker, tasks complete,
resizing remains usable, and destruction drains queued work.

Resizing tests cover the first failed worker creation, failure after partial growth,
growth while a previous downsizing is still in progress, a reserve request above
the vector's maximum size, and 3,000 tasks submitted during repeated resizing.
Each injected failure must leave the pool usable for tasks, resizing, and
destruction. Retirement and task checks use five-second deadlines; the outer
timeout also bounds pool destruction. Checks remain enabled with `NDEBUG`.

Pass `construct`, `construct-reserve`, `zero`, `first`, `partial`, `downsizing`,
`reserve`, or `tasks` as the executable's argument to run a single case. Before
constructor failure cleanup, `construct` can terminate or hang during unwinding.
Against the original resizing implementation, `first` asserts in a debug build;
with assertions disabled, the invalid worker index can crash during shutdown.

The shutdown executable accepts `shutdown-active`, `shutdown-retired`,
`independent`, `worker-resize`, `resize-worker`, `resize-concurrent`, `construct`,
`failed-growth`, or `concurrent` to run a single case. These check:

- Shutdown waiting for TLS cleanup of active and previously retired workers.
- Lower-index workers retiring while the highest-index worker is still busy.
- Worker tasks and TLS destructors resizing without joining pool workers.
- External resizers joining a retired snapshot outside the pool mutex, while
  another caller reuses slots and changes the target. The joining caller must
  reread that target before deciding whether to grow or notify a shrink.
- Constructor failure joining fully before propagating its original exception,
  and growth failure remaining recoverable through retirement and retry.
- Concurrent external resizers completing all queued tasks exactly once.

Synchronization uses gates, join-entry notifications, and ten-second deadlines;
checks stay enabled with `NDEBUG`. `shutdown-active` and `shutdown-retired` report
early destructor return against the previous detaching implementation. To compare
revisions, compile with `-I` pointing to a directory containing that revision's
`ThreadPool.h`, keeping the test source unchanged. The `construct` case similarly
detects constructor failure propagating before TLS cleanup has finished.
