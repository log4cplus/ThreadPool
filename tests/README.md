# ThreadPool construction and resizing regression tests

`worker_failure.cpp` is a standalone Linux test using `pthread_create`
interposition and `dlsym(RTLD_NEXT, ...)`. It injects `EAGAIN` without exhausting
system resources and tracks when partially started workers retire. No production
test hooks or external test framework are required.

Run these commands from the ThreadPool repository root, using GCC or Clang:

```sh
g++ -std=c++11 -O0 -g -Wall -Wextra -Wpedantic -pthread -I. \
    tests/worker_failure.cpp -ldl -o /tmp/threadpool-worker-debug
timeout 60s /tmp/threadpool-worker-debug

g++ -std=c++11 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -pthread -I. \
    tests/worker_failure.cpp -ldl -o /tmp/threadpool-worker-release
timeout 60s /tmp/threadpool-worker-release

g++ -std=c++23 -O1 -g -DNDEBUG -fsanitize=address,undefined \
    -fno-omit-frame-pointer -pthread -I. tests/worker_failure.cpp -ldl \
    -o /tmp/threadpool-worker-sanitized
timeout 60s /tmp/threadpool-worker-sanitized
```

Repeat the C++11 commands with `clang++` in place of `g++`.

If a sandbox prevents LeakSanitizer from inspecting threads at process exit,
prefix the sanitized run with `ASAN_OPTIONS=detect_leaks=0`. AddressSanitizer
and UndefinedBehaviorSanitizer remain enabled.

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
