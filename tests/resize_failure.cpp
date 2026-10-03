// Standalone Linux regression tests. See README.md for build instructions.
#include "ThreadPool.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>

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
    tracker().wait_finished(workers.back());
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
    require(tracker().snapshot().size() == workers.size(),
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
            pool.set_pool_size(std::vector<std::thread>().max_size() + 1);
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

    std::lock_guard<std::mutex> lock(tracker().mutex);
    if (++tracker().attempts == tracker().fail_at)
        return EAGAIN;

    auto worker = std::make_shared<Worker>();
    std::unique_ptr<Start> start(new Start{function, argument, worker});
    tracker().workers.push_back(worker);
    const int result = real_create(thread, attributes, tracked_start, start.get());
    if (result == 0)
        start.release();
    else
        tracker().workers.pop_back();
    return result;
}

int main(int argc, char **argv)
{
    track_creation = true;
    const struct Test {
        const char *name;
        void (*run)();
    } tests[] = {
        {"first", first_creation_failure},
        {"partial", partial_growth_failure},
        {"downsizing", growth_during_downsizing},
        {"reserve", reserve_failure},
        {"tasks", resizing_with_tasks}
    };
    bool ran = false;
    for (const auto &test : tests) {
        if (argc > 1 && std::strcmp(argv[1], test.name) != 0)
            continue;
        ran = true;
        test.run();
        // Pool destruction detaches workers. Keep test bookkeeping alive
        // until their entry functions have actually returned.
        for (const auto &worker : tracker().snapshot())
            tracker().wait_finished(worker);
        std::printf("PASS: %s\n", test.name);
    }
    require(ran, "unknown test name");
}
