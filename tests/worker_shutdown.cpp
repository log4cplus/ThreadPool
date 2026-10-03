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
    std::shared_ptr<Gate> entry_gate;
    std::shared_ptr<Gate> cleanup_gate;
    std::function<void()> cleanup_action;
    std::shared_ptr<Gate> native_cleanup_gate;
    std::function<void()> native_cleanup_action;
    bool cleanup_entered = false;
    bool cleanup_finished = false;
    bool native_cleanup_entered = false;
    bool native_cleanup_finished = false;
    unsigned join_attempts = 0;
    unsigned joins = 0;
    unsigned detaches = 0;
    Worker *joined_by = nullptr;
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
            require(worker->native_cleanup_finished, "native TLS cleanup did not finish");
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
thread_local bool reject_creations = false;
thread_local unsigned creation_attempts = 0;
// A trivial TLS pointer remains usable when cleanup itself creates workers.
thread_local const std::shared_ptr<Gate> *creation_cleanup_gate = nullptr;
thread_local const std::shared_ptr<Gate> *creation_entry_gate = nullptr;
thread_local Worker *current_worker = nullptr;
pthread_key_t native_cleanup_key;

void native_cleanup(void *value)
{
    auto worker = static_cast<Worker *>(value);
    std::shared_ptr<Gate> gate;
    std::function<void()> action;
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        worker->native_cleanup_entered = true;
        gate = worker->native_cleanup_gate;
        action = worker->native_cleanup_action;
        tracker().condition.notify_all();
    }
    if (gate)
        gate->block();
    if (action)
        action();
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        worker->native_cleanup_finished = true;
        tracker().condition.notify_all();
    }
}

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
    require(pthread_setspecific(native_cleanup_key, current_worker) == 0,
        "could not register native TLS cleanup");
    // This destructor runs after the pool's entry function returns. A flag set
    // at the end of that entry function would miss the shutdown bug.
    thread_local Cleanup cleanup{start->worker};
    if (start->worker->entry_gate)
        start->worker->entry_gate->block();
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

void wait_joined(const std::shared_ptr<Worker> &worker)
{
    tracker().wait([&] { return worker->joins == 1; }, "retired worker was not reclaimed");
}

void check_joiner(const std::shared_ptr<Worker> &joiner)
{
    std::lock_guard<std::mutex> lock(tracker().mutex);
    for (const auto &worker : tracker().workers)
        if (worker->join_attempts && worker != joiner)
            require(worker->joined_by == joiner.get(), "worker joined outside the joiner");
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

void shutdown(bool previously_retired, bool native = false)
{
    auto pool = make_pool(previously_retired ? 2 : 1);
    auto worker = tracker().snapshot().back();
    auto gate = std::make_shared<Gate>();
    if (native) {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        worker->native_cleanup_gate = gate;
    } else
        configure_cleanup(worker, gate);
    check_task(*pool);
    if (previously_retired) {
        resize(*pool, 1);
        gate->wait_entered();
    }

    bool returned = false; // Protected by the tracker mutex.
    auto destroying = std::async(std::launch::async, [&] {
        pool.reset();
        std::lock_guard<std::mutex> lock(tracker().mutex);
        returned = true;
        tracker().condition.notify_all();
    });
    tracker().wait([&] {
        return (native ? worker->native_cleanup_entered : worker->cleanup_entered)
            && (worker->join_attempts || returned);
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

    auto joiner = tracker().snapshot().back();
    // The helper may join, but task and cleanup resizers must keep progressing.
    auto task = pool->enqueue([&] { resize(*pool, 3); });
    get_ready(task);
    first->release();
    wait_cleaned(workers[1]); // Must not wait for the other TLS destructor.
    second->release();
    wait_cleaned(workers[2]);
    wait_joined(workers[1]);
    wait_joined(workers[2]);
    check_joiner(joiner);
    resize(*pool, 3);
    check_parallelism(*pool, 3);
}

void resizing_while_joining(bool from_worker, bool native = false)
{
    auto pool = make_pool(3);
    auto workers = tracker().snapshot();
    auto gate = std::make_shared<Gate>();
    auto reenter = [&] {
        pool->set_queue_size_limit(100);
        resize(*pool, 4);
        check_task(*pool); // Cleanup may submit work and wait for an active worker.
    };
    if (native) {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        for (unsigned i = 1; i < 3; ++i) {
            workers[i]->native_cleanup_gate = gate;
            workers[i]->native_cleanup_action = reenter;
        }
    } else {
        configure_cleanup(workers[1], gate, reenter);
        configure_cleanup(workers[2], gate, reenter);
    }
    resize(*pool, 1);
    gate->wait_entered(2);
    auto joiner = tracker().snapshot().back();
    tracker().wait([&] {
        return workers[1]->join_attempts + workers[2]->join_attempts == 1;
    }, "joiner did not reach blocked cleanup");

    auto resizing = std::async(std::launch::async, [&] { resize(*pool, 2); });
    get_ready(resizing);
    auto competing = from_worker
        ? pool->enqueue([&] { resize(*pool, 4); })
        : std::async(std::launch::async, [&] { resize(*pool, 4); });
    get_ready(competing); // Joining must leave queue_mutex available.
    auto grown = tracker().snapshot();
    require(grown.size() == 7, "retired slots were not reused for new workers");
    check_parallelism(*pool, 4);
    gate->release();
    wait_joined(workers[1]);
    wait_joined(workers[2]);
    resize(*pool, 2);
    wait_joined(grown[5]);
    wait_joined(grown[6]);
    check_joiner(joiner);
    check_parallelism(*pool, 2);
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

void fail_resize(progschj::ThreadPool &pool, std::size_t target, unsigned nth)
{
    Tracking tracking;
    const unsigned before = creation_attempts;
    fail_countdown = nth;
    bool rejected = false;
    try {
        pool.set_pool_size(target);
    } catch (const std::system_error &error) {
        rejected = error.code() == std::errc::resource_unavailable_try_again;
    }
    reject_creations = false;
    fail_countdown = 0;
    require(rejected, "resize did not propagate original error");
    require(creation_attempts == before + nth,
        "failure recovery attempted another thread creation");
}

void lazy_startup()
{
    auto pool = make_pool(2);
    require(tracker().snapshot().size() == 2, "constructor started a helper");
    resize(*pool, 2);
    resize(*pool, 4);
    require(tracker().snapshot().size() == 4, "growth or unchanged target started a helper");
    pool.reset();
    require(tracker().snapshot().size() == 4, "destruction started a helper");
    for (const auto &worker : tracker().snapshot())
        require(worker->joined_by == nullptr, "unexpected background join");
}

void shrink_failure(bool from_worker)
{
    auto pool = make_pool(3);
    auto workers = tracker().snapshot();
    if (from_worker) {
        auto shrinking = pool->enqueue([&] { fail_resize(*pool, 1, 1); });
        get_ready(shrinking);
    } else
        fail_resize(*pool, 1, 1);
    require(tracker().snapshot().size() == 3, "failed shrink started another thread");
    check_parallelism(*pool, 3);
    resize(*pool, 1);
    auto joiner = tracker().snapshot().back();
    require(tracker().snapshot().size() == 4, "retry did not start exactly one helper");
    wait_joined(workers[1]);
    wait_joined(workers[2]);
    check_joiner(joiner);
    check_task(*pool);
}

void failed_growth(unsigned nth, bool existing_joiner)
{
    auto pool = make_pool(existing_joiner ? 2 : 1);
    std::shared_ptr<Worker> joiner;
    if (existing_joiner) {
        auto retired = tracker().snapshot().back();
        resize(*pool, 1);
        joiner = tracker().snapshot().back();
        wait_joined(retired);
    }
    auto gate = std::make_shared<Gate>();
    const std::size_t before = tracker().snapshot().size();
    creation_cleanup_gate = &gate;
    fail_resize(*pool, 5, nth);
    creation_cleanup_gate = nullptr;
    auto workers = tracker().snapshot();
    require(workers.size() == before + nth - 1, "unexpected partial growth count");
    std::size_t end = workers.size();
    if (!joiner && nth > 1) {
        joiner = workers.back();
        --end;
    }
    // Everyone except a newly promoted helper must retire against the old target.
    for (std::size_t i = before; i < end; ++i)
        wait_cleanup(workers[i]);
    check_task(*pool);
    resize(*pool, 3); // Must finish even if the helper is joining gated cleanup.
    check_parallelism(*pool, 3);
    gate->release();
    for (std::size_t i = before; i < end; ++i)
        wait_joined(workers[i]);
    resize(*pool, 1);
    if (!joiner)
        joiner = tracker().snapshot().back();
    resize(*pool, 3);
    check_parallelism(*pool, 3);
    pool.reset();
    check_joiner(joiner);
}

void autonomous_cleanup()
{
    auto pool = make_pool(2);
    auto retiring = tracker().snapshot().back();
    std::shared_ptr<Worker> joiner;
    for (unsigned i = 0; i < 30; ++i) {
        auto shrinking = pool->enqueue([&] { resize(*pool, 1); });
        get_ready(shrinking);
        // No external resize/collection call: joining must happen on its own.
        wait_joined(retiring);
        if (!joiner)
            joiner = tracker().snapshot().back();
        check_joiner(joiner);
        auto growing = pool->enqueue([&] { resize(*pool, 2); });
        get_ready(growing);
        retiring = tracker().snapshot().back();
    }
    require(tracker().snapshot().size() == 33, "worker cycles created extra helpers");
    pool.reset();
    check_joiner(joiner);
}

void shutdown_joiner(bool promoted)
{
    auto pool = make_pool(promoted ? 1 : 2);
    if (promoted)
        fail_resize(*pool, 3, 2);
    else {
        auto retiring = tracker().snapshot().back();
        resize(*pool, 1);
        wait_joined(retiring);
    }
    auto joiner = tracker().snapshot().back();
    auto gate = std::make_shared<Gate>();
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        joiner->native_cleanup_gate = gate;
    }
    std::atomic<unsigned> completed(0);
    for (unsigned i = 0; i < 100; ++i)
        pool->enqueue([&] { ++completed; });
    bool returned = false;
    auto destroying = std::async(std::launch::async, [&] {
        pool.reset();
        std::lock_guard<std::mutex> lock(tracker().mutex);
        returned = true;
        tracker().condition.notify_all();
    });
    gate->wait_entered();
    {
        std::lock_guard<std::mutex> lock(tracker().mutex);
        require(!returned && joiner->join_attempts == 1,
            "destruction did not wait for the joiner's complete exit");
        require(completed == 100, "joiner exited before queued tasks were drained");
    }
    gate->release();
    get_ready(destroying);
    check_joiner(joiner);
}

void immediate_promoted_shutdown()
{
    auto pool = make_pool(1);
    auto gate = std::make_shared<Gate>();
    creation_entry_gate = &gate;
    fail_resize(*pool, 3, 2);
    creation_entry_gate = nullptr;
    auto joiner = tracker().snapshot().back();
    gate->wait_entered();
    // Stop the pool before the promoted worker can even read its startup role.
    auto destroying = std::async(std::launch::async, [&] { pool.reset(); });
    tracker().wait([&] { return joiner->join_attempts == 1; },
        "destruction did not join the promoted worker");
    gate->release();
    get_ready(destroying);
    check_joiner(joiner);
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
    // Every background join in this pool must have the same caller, including
    // when concurrent resizers raced to perform its first shrink.
    Worker *joiner = nullptr;
    for (const auto &worker : tracker().snapshot()) {
        if (worker->joined_by) {
            if (!joiner)
                joiner = worker->joined_by;
            require(worker->joined_by == joiner, "multiple joiners were started");
        }
    }
    require(joiner != nullptr, "concurrent resizing did not start a joiner");
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
    ++creation_attempts;
    if (reject_creations || (fail_countdown && --fail_countdown == 0)) {
        reject_creations = true; // Fail every later attempt until explicitly reset.
        return EAGAIN;
    }

    auto worker = std::make_shared<Worker>();
    if (creation_entry_gate)
        worker->entry_gate = *creation_entry_gate;
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
            worker->joined_by = current_worker;
            require(worker.get() != current_worker, "thread attempted to join itself");
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
    require(pthread_key_create(&native_cleanup_key, native_cleanup) == 0,
        "could not create native TLS key");
    const struct Test {
        const char *name;
        void (*run)();
    } tests[] = {
        {"shutdown-active", [] { shutdown(false); }},
        {"shutdown-retired", [] { shutdown(true); }},
        {"shutdown-native-active", [] { shutdown(false, true); }},
        {"shutdown-native-retired", [] { shutdown(true, true); }},
        {"independent", independent_retirement},
        {"worker-resize", worker_resizing},
        {"resize-worker", [] { resizing_while_joining(true); }},
        {"resize-concurrent", [] { resizing_while_joining(false); }},
        {"resize-native", [] { resizing_while_joining(true, true); }},
        {"construct", constructor_failure},
        {"lazy", lazy_startup},
        {"shrink-failure", [] { shrink_failure(false); }},
        {"shrink-worker-failure", [] { shrink_failure(true); }},
        {"failed-first", [] { failed_growth(1, false); }},
        {"failed-growth", [] { failed_growth(2, false); }},
        {"failed-several", [] { failed_growth(4, false); }},
        {"failed-existing", [] { failed_growth(3, true); }},
        {"autonomous", autonomous_cleanup},
        {"shutdown-joiner", [] { shutdown_joiner(false); }},
        {"shutdown-promoted", [] { shutdown_joiner(true); }},
        {"shutdown-immediate", immediate_promoted_shutdown},
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
    require(pthread_key_delete(native_cleanup_key) == 0, "could not delete native TLS key");
}
