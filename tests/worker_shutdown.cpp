// Standalone Linux worker-lifetime tests. See README.md for build instructions.
#include "ThreadPool.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>

namespace {

const std::chrono::seconds deadline(10);

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        // Do not unwind a pool whose workers are deliberately gated.
        std::_Exit(EXIT_FAILURE);
    }
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

    void wait_entered(std::size_t count = 1)
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, deadline,
            [&] { return entered == count; }), "workers did not reach gate");
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};

struct Worker {
    pthread_t handle;
    Gate task_gate;
    std::shared_ptr<Gate> cleanup_gate;
    std::function<void()> cleanup_action;
    bool cleanup_entered = false;
    bool cleanup_finished = false;
    unsigned join_attempts = 0;
    unsigned joins = 0;
    unsigned detaches = 0;
};

struct Tracker {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::shared_ptr<Worker> > workers;

    template <class Predicate>
    void wait(Predicate predicate, const char *message)
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, deadline, predicate), message);
    }

    std::vector<std::shared_ptr<Worker> > snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return workers;
    }

    // Called with mutex held. Prefer the newest record if pthread IDs have
    // been reused after a completed join.
    std::shared_ptr<Worker> find(pthread_t handle)
    {
        for (auto it = workers.rbegin(); it != workers.rend(); ++it)
            if (!(*it)->joins && !(*it)->detaches
                && pthread_equal((*it)->handle, handle))
                return *it;
        return std::shared_ptr<Worker>();
    }

    void finish_test()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto &worker : workers) {
            require(worker->cleanup_finished, "worker TLS cleanup did not finish");
            require(worker->join_attempts == 1 && worker->joins == 1,
                "worker was not joined exactly once");
            require(worker->detaches == 0, "worker was detached");
        }
        workers.clear();
    }
};

Tracker &tracker()
{
    static Tracker instance;
    return instance;
}

// Instrument pool creations only, not the test's async control threads.
thread_local bool track_creation = false;
thread_local std::size_t fail_countdown = 0;
// A trivial TLS pointer remains usable when cleanup itself creates workers.
thread_local const std::shared_ptr<Gate> *creation_cleanup_gate = nullptr;
thread_local Worker *current_worker = nullptr;

struct Tracking {
    bool previous;
    Tracking() : previous(track_creation) { track_creation = true; }
    ~Tracking() { track_creation = previous; }
};

struct Cleanup {
    std::shared_ptr<Worker> worker;

    ~Cleanup()
    {
        std::shared_ptr<Gate> gate;
        std::function<void()> action;
        {
            std::lock_guard<std::mutex> lock(tracker().mutex);
            worker->cleanup_entered = true;
            gate = worker->cleanup_gate;
            action = worker->cleanup_action;
            tracker().condition.notify_all();
        }
        if (gate)
            gate->block();
        if (action)
            action();
        {
            std::lock_guard<std::mutex> lock(tracker().mutex);
            worker->cleanup_finished = true;
            tracker().condition.notify_all();
        }
    }
};

struct Start {
    void *(*function)(void *);
    void *argument;
    std::shared_ptr<Worker> worker;
};

void *tracked_start(void *argument)
{
    std::unique_ptr<Start> start(static_cast<Start *>(argument));
    track_creation = true;
    current_worker = start->worker.get();
    // This destructor runs after the pool's entry function returns. A flag set
    // at the end of that entry function would miss the shutdown bug.
    thread_local Cleanup cleanup{start->worker};
    return start->function(start->argument);
}

template <class T>
T get_ready(std::future<T> &future)
{
    require(future.wait_for(deadline) == std::future_status::ready,
        "operation did not finish");
    return future.get();
}

std::unique_ptr<progschj::ThreadPool> make_pool(std::size_t size)
{
    Tracking tracking;
    return std::unique_ptr<progschj::ThreadPool>(new progschj::ThreadPool(size));
}

void resize(progschj::ThreadPool &pool, std::size_t size)
{
    Tracking tracking;
    pool.set_pool_size(size);
}

void configure_cleanup(const std::shared_ptr<Worker> &worker,
    const std::shared_ptr<Gate> &gate,
    std::function<void()> action = std::function<void()>())
{
    std::lock_guard<std::mutex> lock(tracker().mutex);
    require(!worker->cleanup_entered, "cleanup configured too late");
    worker->cleanup_gate = gate;
    worker->cleanup_action = std::move(action);
}

void wait_cleanup(const std::shared_ptr<Worker> &worker)
{
    tracker().wait([&] { return worker->cleanup_entered; },
        "worker did not retire independently");
}

void wait_cleaned(const std::shared_ptr<Worker> &worker)
{
    tracker().wait([&] { return worker->cleanup_finished; },
        "worker cleanup did not finish");
}

void check_task(progschj::ThreadPool &pool)
{
    auto task = pool.enqueue([] { return 42; });
    require(get_ready(task) == 42, "incorrect task result");
}

void check_parallelism(progschj::ThreadPool &pool, std::size_t size)
{
    auto gate = std::make_shared<Gate>();
    std::vector<std::future<void> > tasks;
    for (std::size_t i = 0; i < size; ++i)
        tasks.push_back(pool.enqueue([gate] { gate->block(); }));
    gate->wait_entered(size);
    gate->release();
    for (auto &task : tasks)
        get_ready(task);
}

void shutdown(bool previously_retired)
{
    auto pool = make_pool(previously_retired ? 2 : 1);
    auto worker = tracker().snapshot().back();
    auto gate = std::make_shared<Gate>();
    configure_cleanup(worker, gate);
    check_task(*pool);
    if (previously_retired) {
        resize(*pool, 1);
        wait_cleanup(worker);
    }

    bool returned = false; // Protected by the tracker mutex.
    auto destroying = std::async(std::launch::async, [&] {
        pool.reset();
        std::lock_guard<std::mutex> lock(tracker().mutex);
        returned = true;
        tracker().condition.notify_all();
    });
    tracker().wait([&] {
        return worker->cleanup_entered && (worker->join_attempts || returned);
    }, "destruction did not reach worker cleanup");
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        require(!returned, "destructor returned before worker TLS cleanup finished");
        require(worker->join_attempts == 1, "destructor did not join gated worker");
    }
    gate->release();
    get_ready(destroying);
}

void independent_retirement()
{
    auto pool = make_pool(3);
    auto workers = tracker().snapshot();
    std::vector<std::future<void> > tasks;
    for (unsigned i = 0; i < 3; ++i)
        tasks.push_back(pool->enqueue([] { current_worker->task_gate.block(); }));
    for (auto &worker : workers)
        worker->task_gate.wait_entered();
    resize(*pool, 1);
    workers[1]->task_gate.release();
    wait_cleanup(workers[1]);
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        require(!workers[2]->cleanup_entered,
            "highest-index worker unexpectedly left its task");
    }
    workers[2]->task_gate.release();
    workers[0]->task_gate.release();
    for (auto &task : tasks)
        get_ready(task);
}

void worker_resizing()
{
    auto pool = make_pool(3);
    auto workers = tracker().snapshot();
    auto first = std::make_shared<Gate>();
    auto second = std::make_shared<Gate>();
    auto reenter = [&] {
        pool->set_queue_size_limit(100);
        resize(*pool, 3);
    };
    configure_cleanup(workers[1], first, reenter);
    configure_cleanup(workers[2], second, reenter);
    resize(*pool, 1);
    wait_cleanup(workers[1]);
    wait_cleanup(workers[2]);

    // No external caller has claimed these records. A worker-origin resize
    // must skip all of them, not merely its own thread handle.
    auto task = pool->enqueue([&] { resize(*pool, 3); });
    get_ready(task);
    first->release();
    wait_cleaned(workers[1]); // Must not wait for the other TLS destructor.
    second->release();
    wait_cleaned(workers[2]);
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        require(workers[1]->join_attempts == 0 && workers[2]->join_attempts == 0,
            "pool worker attempted to join another pool worker");
    }
    resize(*pool, 3);
    check_parallelism(*pool, 3);
}

void resizing_while_joining(bool from_worker)
{
    auto pool = make_pool(3);
    auto workers = tracker().snapshot();
    auto gate = std::make_shared<Gate>();
    auto reenter = [&] {
        pool->set_queue_size_limit(100);
        resize(*pool, 4);
    };
    configure_cleanup(workers[1], gate, reenter);
    configure_cleanup(workers[2], gate, reenter);
    resize(*pool, 1);
    wait_cleanup(workers[1]);
    wait_cleanup(workers[2]);

    auto resizing = std::async(std::launch::async, [&] { resize(*pool, 2); });
    tracker().wait([&] { return workers[1]->join_attempts == 1; },
        "resize did not join retired worker");
    auto competing = from_worker
        ? pool->enqueue([&] { resize(*pool, 4); })
        : std::async(std::launch::async, [&] { resize(*pool, 4); });
    get_ready(competing); // Joining must leave queue_mutex available.
    auto grown = tracker().snapshot();
    require(grown.size() == 6, "retired slots were not reused for new workers");
    require(resizing.wait_for(std::chrono::seconds(0)) == std::future_status::timeout,
        "resize returned while retired worker TLS cleanup was blocked");
    gate->release();
    get_ready(resizing);

    // The first caller must reread the target (now 4) and notify a shrink to 2.
    // Using its stale target of 1 would leave these idle workers asleep.
    wait_cleanup(grown[4]);
    wait_cleanup(grown[5]);
    check_parallelism(*pool, 2);
    resize(*pool, 2);
}

void constructor_failure()
{
    auto gate = std::make_shared<Gate>();
    bool returned = false;
    auto constructing = std::async(std::launch::async, [&] {
        Tracking tracking;
        fail_countdown = 2;
        creation_cleanup_gate = &gate;
        bool rejected = false;
        try {
            progschj::ThreadPool pool(2);
        } catch (const std::system_error &error) {
            rejected = error.code() == std::errc::resource_unavailable_try_again;
        }
        std::lock_guard<std::mutex> lock(tracker().mutex);
        returned = true;
        tracker().condition.notify_all();
        return rejected;
    });
    tracker().wait([&] { return !tracker().workers.empty(); },
        "constructor did not start its first worker");
    auto worker = tracker().snapshot().front();
    tracker().wait([&] {
        return worker->cleanup_entered && (worker->join_attempts || returned);
    }, "constructor cleanup did not reach TLS destruction");
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        require(!returned, "constructor propagated failure before complete worker exit");
    }
    gate->release();
    require(get_ready(constructing), "constructor did not propagate original error");
}

void failed_growth()
{
    auto pool = make_pool(1);
    auto gate = std::make_shared<Gate>();
    auto failing = std::async(std::launch::async, [&] {
        Tracking tracking;
        fail_countdown = 2;
        creation_cleanup_gate = &gate;
        try {
            pool->set_pool_size(4);
        } catch (const std::system_error &error) {
            return error.code() == std::errc::resource_unavailable_try_again;
        }
        return false;
    });
    require(get_ready(failing), "growth did not propagate original error");
    auto workers = tracker().snapshot();
    require(workers.size() == 2, "expected one partially started worker");
    wait_cleanup(workers[1]);
    auto retrying = std::async(std::launch::async, [&] { resize(*pool, 3); });
    tracker().wait([&] { return workers[1]->join_attempts == 1; },
        "retry did not reclaim partially started worker");
    check_task(*pool);
    gate->release();
    get_ready(retrying);
    check_parallelism(*pool, 3);
}

void concurrent_resizing()
{
    auto pool = make_pool(2);
    const unsigned callers = 3, batches = 30, batch_size = 4;
    std::atomic<unsigned> counts[callers * batches * batch_size];
    for (auto &count : counts)
        count.store(0);
    std::vector<std::future<void> > resizers;
    for (unsigned caller = 0; caller < callers; ++caller) {
        resizers.push_back(std::async(std::launch::async, [&, caller] {
            std::vector<std::future<void> > tasks;
            for (unsigned batch = 0; batch < batches; ++batch) {
                resize(*pool, 1 + (batch + caller) % 6);
                for (unsigned j = 0; j < batch_size; ++j) {
                    const unsigned index = (caller * batches + batch) * batch_size + j;
                    tasks.push_back(pool->enqueue([&, index] { ++counts[index]; }));
                }
            }
            for (auto &task : tasks)
                get_ready(task);
        }));
    }
    for (auto &resizer : resizers)
        get_ready(resizer);
    pool.reset();
    for (auto &count : counts)
        require(count == 1, "task did not execute exactly once during concurrent resizing");
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
    if (fail_countdown && --fail_countdown == 0)
        return EAGAIN;

    auto worker = std::make_shared<Worker>();
    if (creation_cleanup_gate)
        worker->cleanup_gate = *creation_cleanup_gate;
    std::unique_ptr<Start> start(new Start{function, argument, worker});
    std::lock_guard<std::mutex> lock(tracker().mutex);
    tracker().workers.push_back(worker);
    const int result = real_create(thread, attributes, tracked_start, start.get());
    if (result == 0) {
        worker->handle = *thread;
        start.release();
    } else
        tracker().workers.pop_back();
    tracker().condition.notify_all();
    return result;
}

extern "C" int pthread_join(pthread_t thread, void **result)
{
    static auto real_join = reinterpret_cast<decltype(&pthread_join)>(
        dlsym(RTLD_NEXT, "pthread_join"));
    require(real_join != nullptr, "could not resolve pthread_join");
    std::shared_ptr<Worker> worker;
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        worker = tracker().find(thread);
        if (worker) {
            require(++worker->join_attempts == 1, "duplicate concurrent join");
            tracker().condition.notify_all();
        }
    }
    const int error = real_join(thread, result);
    if (worker && error == 0) {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        ++worker->joins;
        tracker().condition.notify_all();
    }
    return error;
}

extern "C" int pthread_detach(pthread_t thread) noexcept
{
    static auto real_detach = reinterpret_cast<decltype(&pthread_detach)>(
        dlsym(RTLD_NEXT, "pthread_detach"));
    require(real_detach != nullptr, "could not resolve pthread_detach");
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        auto worker = tracker().find(thread);
        if (worker)
            ++worker->detaches;
    }
    return real_detach(thread);
}

int main(int argc, char **argv)
{
    const struct Test {
        const char *name;
        void (*run)();
    } tests[] = {
        {"shutdown-active", [] { shutdown(false); }},
        {"shutdown-retired", [] { shutdown(true); }},
        {"independent", independent_retirement},
        {"worker-resize", worker_resizing},
        {"resize-worker", [] { resizing_while_joining(true); }},
        {"resize-concurrent", [] { resizing_while_joining(false); }},
        {"construct", constructor_failure},
        {"failed-growth", failed_growth},
        {"concurrent", concurrent_resizing}
    };
    bool ran = false;
    for (const auto &test : tests) {
        if (argc > 1 && std::strcmp(argv[1], test.name) != 0)
            continue;
        ran = true;
        test.run();
        tracker().finish_test();
        std::printf("PASS: %s\n", test.name);
    }
    require(ran, "unknown test name");
}
