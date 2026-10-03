// Standalone Linux worker-failure tests. See README.md for build instructions.
#include "ThreadPool.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <new>

// Fail only allocations on the resizing caller. pthread_create bookkeeping is
// excluded below, so exceptions cannot cross the interposed C API boundary.
thread_local std::size_t allocation_countdown = 0;
thread_local unsigned allocation_rejections = 0;

__attribute__((noinline)) void *operator new(std::size_t size)
{
    if (allocation_rejections
        || (allocation_countdown && --allocation_countdown == 0)) {
        ++allocation_rejections;
        throw std::bad_alloc();
    }
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}

// Keep replacement allocation functions out of line so GCC does not diagnose
// their malloc/free implementation as mismatched new/delete at call sites.
__attribute__((noinline)) void operator delete(void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void *p, std::size_t) noexcept { std::free(p); }

namespace {

const std::chrono::seconds deadline(5);

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        // A failed assertion must not hang while unwinding a gated pool.
        std::_Exit(EXIT_FAILURE);
    }
}

struct Worker {
    bool finished = false;
};

struct Tracker {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t attempts = 0;
    std::size_t fail_at = 0;
    std::vector<std::shared_ptr<Worker> > workers;

    std::vector<std::shared_ptr<Worker> > snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return workers;
    }

    std::size_t attempt_count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return attempts;
    }

    void wait_finished(const std::shared_ptr<Worker> &worker)
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, deadline,
            [&] { return worker->finished; }), "worker did not retire");
    }
};

Tracker &tracker()
{
    static Tracker instance;
    return instance;
}

// Only creations requested by the test's main thread are instrumented.
// Other library/runtime threads, including sanitizer helpers, pass through.
thread_local bool track_creation = false;

struct Start {
    void *(*function)(void *);
    void *argument;
    std::shared_ptr<Worker> worker;
};

void *tracked_start(void *argument)
{
    std::unique_ptr<Start> start(static_cast<Start *>(argument));
    std::shared_ptr<Worker> worker = start->worker;
    void *result = start->function(start->argument);
    start.reset();
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        worker->finished = true;
        tracker().condition.notify_all();
    }
    return result;
}

class FailCreation {
public:
    explicit FailCreation(std::size_t nth)
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        tracker().fail_at = tracker().attempts + nth;
    }

    ~FailCreation()
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        tracker().fail_at = 0;
    }
};

void fail_growth(progschj::ThreadPool &pool, std::size_t target,
    std::size_t nth = 1)
{
    FailCreation failure(nth);
    try {
        pool.set_pool_size(target);
    } catch (const std::system_error &error) {
        require(error.code() == std::errc::resource_unavailable_try_again,
            "thread creation error was not propagated unchanged");
        return;
    }
    require(false, "growth did not throw the injected error");
}

template <class T>
T get_ready(std::future<T> &future)
{
    require(future.wait_for(deadline) == std::future_status::ready,
        "task did not finish");
    return future.get();
}

void check_task(progschj::ThreadPool &pool)
{
    auto future = pool.enqueue([] { return 42; });
    require(get_ready(future) == 42, "task returned the wrong value");
}

struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t entered = 0;
    bool released = false;

    void block()
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++entered;
        condition.notify_all();
        require(condition.wait_for(lock, deadline, [&] { return released; }),
            "gate was not released");
    }

    void wait_entered(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, deadline,
            [&] { return entered == count; }), "not all workers reached gate");
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};

std::vector<std::future<void> > occupy(progschj::ThreadPool &pool,
    const std::shared_ptr<Gate> &gate, std::size_t count)
{
    std::vector<std::future<void> > futures;
    for (std::size_t i = 0; i < count; ++i)
        futures.push_back(pool.enqueue([gate] { gate->block(); }));
    gate->wait_entered(count);
    return futures;
}

void check_parallelism(progschj::ThreadPool &pool, std::size_t count)
{
    auto gate = std::make_shared<Gate>();
    auto futures = occupy(pool, gate, count);
    gate->release();
    for (auto &future : futures)
        get_ready(future);
}

void construction_failure()
{
    // Cover no workers started, one worker started, and several started.
    const std::size_t failures[] = {1, 2, 4};
    for (std::size_t nth : failures) {
        const std::size_t before = tracker().snapshot().size();
        bool rejected = false;
        {
            FailCreation failure(nth);
            try {
                progschj::ThreadPool pool(4);
            } catch (const std::system_error &error) {
                require(error.code() == std::errc::resource_unavailable_try_again,
                    "constructor did not propagate the thread creation error");
                rejected = true;
            }
        }
        require(rejected, "constructor did not throw the injected error");
        auto workers = tracker().snapshot();
        require(workers.size() == before + nth - 1,
            "unexpected number of workers started before construction failed");
        for (std::size_t i = before; i < workers.size(); ++i)
            tracker().wait_finished(workers[i]);

        // The exception must be recoverable, including building another pool.
        progschj::ThreadPool recovered(2);
        check_parallelism(recovered, 2);
    }
}

void construction_reserve_failure()
{
    const std::size_t attempts = tracker().attempt_count();
    bool rejected = false;
    {
        // Bound the unfixed implementation without exhausting real resources.
        FailCreation failure(1);
        try {
            progschj::ThreadPool pool(std::vector<void *>().max_size() + 1);
        } catch (const std::length_error &) {
            rejected = true;
        } catch (...) {
            require(false, "oversized construction did not fail in reserve");
        }
    }
    require(rejected, "oversized construction was not rejected");
    require(tracker().attempt_count() == attempts,
        "worker creation happened before construction reserve failed");
    progschj::ThreadPool recovered(2);
    check_parallelism(recovered, 2);
}

void zero_worker_construction()
{
    const std::size_t before = tracker().snapshot().size();
    std::atomic<unsigned> completed(0);
    {
        progschj::ThreadPool pool(0);
        check_task(pool);
        require(tracker().snapshot().size() == before + 1,
            "zero construction did not start exactly one worker");
        pool.set_pool_size(3);
        check_parallelism(pool, 3);
        pool.set_pool_size(0);
        check_task(pool);
        for (unsigned i = 0; i < 100; ++i)
            pool.enqueue([&completed] { ++completed; });
    }
    require(completed == 100, "zero-constructed pool did not drain its tasks");
}

void first_creation_failure()
{
    progschj::ThreadPool pool(1);
    fail_growth(pool, 3);
    // This is the original failure: retrying used the unfulfilled target
    // as a worker index, causing an assertion or an invalid detach.
    pool.set_pool_size(4);
    check_parallelism(pool, 4);
    pool.set_pool_size(1);
    check_task(pool);
}

void partial_growth_failure()
{
    progschj::ThreadPool pool(1);
    const std::size_t before = tracker().snapshot().size();
    fail_growth(pool, 4, 2);
    auto workers = tracker().snapshot();
    require(workers.size() == before + 1, "expected one partially added worker");
    // The partially started worker becomes the joiner until pool destruction.
    check_task(pool);
    pool.set_pool_size(4);
    check_parallelism(pool, 4);
    pool.set_pool_size(1);
    pool.set_pool_size(3);
    check_parallelism(pool, 3);
}

void growth_during_downsizing()
{
    const std::size_t before = tracker().snapshot().size();
    progschj::ThreadPool pool(3);
    auto workers = tracker().snapshot();
    auto gate = std::make_shared<Gate>();
    auto futures = occupy(pool, gate, 3);
    pool.set_pool_size(1);

    // Workers 1 and 2 are still executing. Growth must reuse them before
    // attempting (and failing) to create worker 3.
    const std::size_t attempts = tracker().attempt_count();
    fail_growth(pool, 4);
    require(tracker().attempt_count() == attempts + 1,
        "growth did not reuse the workers still executing");
    require(tracker().snapshot().size() == workers.size() + 1,
        "growth unexpectedly created another worker");
    gate->release();
    for (auto &future : futures)
        get_ready(future);
    tracker().wait_finished(workers[before + 1]);
    tracker().wait_finished(workers[before + 2]);
    check_task(pool);
    pool.set_pool_size(3);
    check_parallelism(pool, 3);
}

void reserve_failure()
{
    progschj::ThreadPool pool(1);
    const std::size_t attempts = tracker().attempt_count();
    bool rejected = false;
    {
        // Also cap the unfixed implementation: it must not try to exhaust
        // real system resources when it misses the reserve check.
        FailCreation failure(1);
        try {
            pool.set_pool_size(std::vector<void *>().max_size() + 1);
        } catch (const std::length_error &) {
            rejected = true;
        } catch (...) {
            require(false, "oversized growth did not fail in reserve");
        }
    }
    require(rejected, "oversized growth was not rejected");
    require(tracker().attempt_count() == attempts,
        "worker creation happened before reserve failed");
    check_task(pool);
    pool.set_pool_size(3);
    check_parallelism(pool, 3);
}

void allocation_failure()
{
    {
        progschj::ThreadPool pool(3);
        const std::size_t attempts = tracker().attempt_count();
        allocation_countdown = 1;
        bool rejected = false;
        try {
            pool.set_pool_size(1);
        } catch (const std::bad_alloc &) {
            rejected = true;
        }
        const unsigned rejections = allocation_rejections;
        allocation_countdown = 0;
        allocation_rejections = 0;
        require(rejected && rejections == 1, "helper allocation failure was not propagated");
        require(tracker().attempt_count() == attempts,
            "helper creation followed failed allocation");
        check_parallelism(pool, 3);
        pool.set_pool_size(1);
        check_task(pool);
    }
    unsigned before_start = 0, after_start = 0;
    // Cover reserve, record allocation, and thread-state allocation, including
    // failures after one or more workers have started. Further allocations
    // keep failing until the resize has propagated its original exception.
    for (std::size_t nth = 1; nth <= 12; ++nth) {
        progschj::ThreadPool pool(1);
        const std::size_t before = tracker().snapshot().size();
        allocation_countdown = nth;
        bool rejected = false;
        try {
            pool.set_pool_size(4);
        } catch (const std::bad_alloc &) {
            rejected = true;
        }
        const unsigned rejections = allocation_rejections;
        allocation_countdown = 0;
        allocation_rejections = 0;
        if (rejected) {
            require(rejections == 1, "failure recovery attempted another allocation");
            auto workers = tracker().snapshot();
            if (workers.size() == before)
                ++before_start;
            else {
                ++after_start;
                // The last addition becomes the helper; earlier ones retire.
                for (std::size_t i = before; i + 1 < workers.size(); ++i)
                    tracker().wait_finished(workers[i]);
            }
            check_task(pool);
        }
        pool.set_pool_size(4);
        check_parallelism(pool, 4);
        pool.set_pool_size(1);
        check_task(pool);
    }
    require(before_start && after_start, "allocation injection missed partial growth");
}

void resizing_with_tasks()
{
    const std::size_t batches = 100;
    const std::size_t batch_size = 30;
    std::unique_ptr<std::atomic<unsigned>[]> counts(
        new std::atomic<unsigned>[batches * batch_size]);
    for (std::size_t i = 0; i < batches * batch_size; ++i)
        counts[i].store(0);
    {
        progschj::ThreadPool pool(2);
        for (std::size_t batch = 0; batch < batches; ++batch) {
            pool.set_pool_size(1 + batch % 6);
            for (std::size_t j = 0; j < batch_size; ++j) {
                const std::size_t i = batch * batch_size + j;
                pool.enqueue([&counts, i] { ++counts[i]; });
            }
        }
        // Destruction must finish and drain all queued work.
    }
    for (std::size_t i = 0; i < batches * batch_size; ++i)
        require(counts[i] == 1, "queued task did not execute exactly once");
}

} // namespace

extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
    void *(*function)(void *), void *argument)
{
    static auto real_create = reinterpret_cast<decltype(&pthread_create)>(
        dlsym(RTLD_NEXT, "pthread_create"));
    require(real_create != nullptr, "could not resolve pthread_create");
    if (!track_creation)
        return real_create(thread, attributes, function, argument);

    const std::size_t saved_countdown = allocation_countdown;
    allocation_countdown = 0;
    std::lock_guard<std::mutex> lock(tracker().mutex);
    if (++tracker().attempts == tracker().fail_at) {
        allocation_countdown = saved_countdown;
        return EAGAIN;
    }

    auto worker = std::make_shared<Worker>();
    std::unique_ptr<Start> start(new Start{function, argument, worker});
    tracker().workers.push_back(worker);
    const int result = real_create(thread, attributes, tracked_start, start.get());
    if (result == 0)
        start.release();
    else
        tracker().workers.pop_back();
    allocation_countdown = saved_countdown;
    return result;
}

int main(int argc, char **argv)
{
    track_creation = true;
    const struct Test {
        const char *name;
        void (*run)();
    } tests[] = {
        {"construct", construction_failure},
        {"construct-reserve", construction_reserve_failure},
        {"zero", zero_worker_construction},
        {"first", first_creation_failure},
        {"partial", partial_growth_failure},
        {"downsizing", growth_during_downsizing},
        {"reserve", reserve_failure},
        {"allocation", allocation_failure},
        {"tasks", resizing_with_tasks}
    };
    bool ran = false;
    for (const auto &test : tests) {
        if (argc > 1 && std::strcmp(argv[1], test.name) != 0)
            continue;
        ran = true;
        test.run();
        // Also keep bookkeeping alive when testing older, detaching revisions.
        for (const auto &worker : tracker().snapshot())
            tracker().wait_finished(worker);
        std::printf("PASS: %s\n", test.name);
    }
    require(ran, "unknown test name");
}
