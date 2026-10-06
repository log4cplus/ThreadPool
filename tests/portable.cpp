// Public-API regression tests for Linux, Windows, and macOS. C++11 throughout.
#include "ThreadPool.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const std::chrono::seconds deadline(10);
const std::chrono::milliseconds blocked_interval(100);

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        // Do not unwind a pool whose tasks or TLS destructors are gated.
        std::abort();
    }
}

template <class T>
T get_ready(std::future<T> &future)
{
    require(future.wait_for(deadline) == std::future_status::ready,
        "future did not become ready");
    return future.get();
}

template <class T>
void require_blocked(std::future<T> &future, const char *message)
{
    require(future.wait_for(blocked_interval) == std::future_status::timeout,
        message);
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
        require(condition.wait_for(lock, deadline, [this] { return released; }),
            "gate was not released");
    }

    void wait_entered(std::size_t count = 1)
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, deadline,
            [this, count] { return entered >= count; }), "task did not reach gate");
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
    std::vector<std::future<void> > tasks;
    for (std::size_t i = 0; i < count; ++i)
        tasks.push_back(pool.enqueue([gate] { gate->block(); }));
    gate->wait_entered(count);
    return tasks;
}

void finish(std::vector<std::future<void> > &tasks)
{
    for (auto &task : tasks)
        get_ready(task);
}

void check_parallelism(progschj::ThreadPool &pool, std::size_t count)
{
    auto gate = std::make_shared<Gate>();
    auto tasks = occupy(pool, gate, count);
    gate->release();
    finish(tasks);
}

void results()
{
    progschj::ThreadPool pool(2);
    auto sum = pool.enqueue([](int left, int right) { return left + right; }, 20, 22);
    require(get_ready(sum) == 42, "bound arguments or result were lost");
    int value = 0;
    auto reference = pool.enqueue([](int &output) { output = 42; }, std::ref(value));
    get_ready(reference);
    require(value == 42, "reference argument was not forwarded");
    auto throwing = pool.enqueue([]() -> int { throw std::runtime_error("task error"); });
    bool caught = false;
    try {
        get_ready(throwing);
    } catch (const std::runtime_error &error) {
        caught = std::strcmp(error.what(), "task error") == 0;
    }
    require(caught, "task exception was not propagated unchanged");
    auto recovered = pool.enqueue([] { return 7; });
    require(get_ready(recovered) == 7, "task exception stopped the worker");
}

void zero()
{
    progschj::ThreadPool pool(0);
    auto first = pool.enqueue([] { return 42; });
    require(get_ready(first) == 42, "zero-size construction created no usable worker");
    pool.set_pool_size(0);
    auto gate = std::make_shared<Gate>();
    auto tasks = occupy(pool, gate, 1);
    auto queued = pool.enqueue([] { return 7; });
    require_blocked(queued, "zero-size request created more than one worker");
    gate->release();
    finish(tasks);
    require(get_ready(queued) == 7, "zero-size resize stopped task execution");
}

void resize()
{
    progschj::ThreadPool pool(1);
    const std::size_t sizes[] = {4, 1, 3, 2, 1, 4};
    for (std::size_t size : sizes) {
        pool.set_pool_size(size);
        check_parallelism(pool, size);
    }
}

void busy_resize()
{
    progschj::ThreadPool pool(3);
    auto gate = std::make_shared<Gate>();
    auto tasks = occupy(pool, gate, 3);
    pool.set_pool_size(1);
    pool.set_pool_size(3);
    auto queued = pool.enqueue([] { return 42; });
    gate->release();
    finish(tasks);
    require(get_ready(queued) == 42, "busy resize lost queued work");
    check_parallelism(pool, 3);
}

void worker_resize()
{
    progschj::ThreadPool pool(3);
    std::atomic<unsigned> completed(0);
    auto resizing = pool.enqueue([&] {
        for (unsigned round = 0; round < 30; ++round) {
            pool.set_pool_size(1);
            pool.set_pool_size(4);
            pool.enqueue([&] { ++completed; });
        }
    });
    get_ready(resizing);
    pool.wait_until_nothing_in_flight();
    require(completed == 30, "worker-origin resize lost work");
    check_parallelism(pool, 4);
}

void concurrent()
{
    const unsigned callers = 4;
    const unsigned batches = 20;
    const unsigned batch_size = 12;
    std::atomic<unsigned> counts[callers * batches * batch_size];
    for (auto &count : counts)
        count.store(0);
    {
        progschj::ThreadPool pool(2);
        std::vector<std::future<void> > resizers;
        for (unsigned caller = 0; caller < callers; ++caller) {
            resizers.push_back(std::async(std::launch::async, [&, caller] {
                std::vector<std::future<void> > tasks;
                for (unsigned batch = 0; batch < batches; ++batch) {
                    pool.set_pool_size(1 + (batch + caller) % 6);
                    for (unsigned j = 0; j < batch_size; ++j) {
                        const unsigned index = (caller * batches + batch) * batch_size + j;
                        tasks.push_back(pool.enqueue([&, index] { ++counts[index]; }));
                    }
                }
                finish(tasks);
            }));
        }
        finish(resizers);
    }
    for (auto &count : counts)
        require(count == 1, "concurrent resize did not execute each task exactly once");
}

void drain()
{
    const unsigned task_count = 1000;
    std::atomic<unsigned> counts[task_count];
    for (auto &count : counts)
        count.store(0);
    std::vector<std::future<unsigned> > tasks;
    {
        progschj::ThreadPool pool(3);
        for (unsigned i = 0; i < task_count; ++i) {
            tasks.push_back(pool.enqueue([&, i] {
                ++counts[i];
                return i;
            }));
        }
    }
    for (unsigned i = 0; i < task_count; ++i) {
        require(counts[i] == 1, "destruction did not drain each task exactly once");
        require(get_ready(tasks[i]) == i, "drained task returned the wrong result");
    }
}

void queue_full()
{
    progschj::ThreadPool pool(1);
    pool.set_queue_size_limit(0); // A zero limit normalizes to one queued task.
    auto gate = std::make_shared<Gate>();
    auto tasks = occupy(pool, gate, 1);
    auto queued = pool.enqueue([] { return 42; });
    auto rejected = pool.enqueue([] { return 7; });
    bool caught = false;
    try {
        get_ready(rejected);
    } catch (const progschj::would_block &) {
        caught = true;
    }
    require(caught, "full queue did not return a would_block future");
    gate->release();
    finish(tasks);
    require(get_ready(queued) == 42, "full queue lost its accepted task");
}

void enqueue_block(bool grow_queue)
{
    progschj::ThreadPool pool(1);
    pool.set_queue_size_limit(1);
    auto gate = std::make_shared<Gate>();
    auto tasks = occupy(pool, gate, 1);
    auto queued = pool.enqueue([] { return 7; });
    std::promise<void> entered;
    auto started = entered.get_future();
    auto producer = std::async(std::launch::async, [&] {
        entered.set_value();
        return pool.enqueue_block([] { return 42; });
    });
    get_ready(started);
    require_blocked(producer, "blocking enqueue returned while the queue was full");
    if (grow_queue)
        pool.set_queue_size_limit(2);
    else
        gate->release();
    auto submitted = get_ready(producer);
    if (grow_queue) {
        require_blocked(submitted, "queue growth released the occupied worker");
        gate->release();
    }
    finish(tasks);
    require(get_ready(queued) == 7 && get_ready(submitted) == 42,
        "blocking enqueue lost a task");
}

void wait_empty()
{
    progschj::ThreadPool pool(1);
    auto first_gate = std::make_shared<Gate>();
    auto tasks = occupy(pool, first_gate, 1);
    auto last_gate = std::make_shared<Gate>();
    auto last = pool.enqueue([last_gate] { last_gate->block(); });
    std::promise<void> entered;
    auto started = entered.get_future();
    auto waiting = std::async(std::launch::async, [&] {
        entered.set_value();
        pool.wait_until_empty();
    });
    get_ready(started);
    require_blocked(waiting, "wait_until_empty returned with queued work");
    first_gate->release();
    last_gate->wait_entered();
    get_ready(waiting); // Empty queue is distinct from all tasks completing.
    require_blocked(last, "last task unexpectedly left its gate");
    last_gate->release();
    finish(tasks);
    get_ready(last);
}

void wait_idle()
{
    progschj::ThreadPool pool(2);
    auto gate = std::make_shared<Gate>();
    auto tasks = occupy(pool, gate, 2);
    std::promise<void> entered;
    auto started = entered.get_future();
    auto waiting = std::async(std::launch::async, [&] {
        entered.set_value();
        pool.wait_until_nothing_in_flight();
    });
    get_ready(started);
    require_blocked(waiting, "idle wait returned while tasks were executing");
    gate->release();
    finish(tasks);
    get_ready(waiting);
}

struct CleanupState {
    Gate gate;
    std::atomic<unsigned> finished;

    CleanupState() : finished(0) {}
};

struct Cleanup {
    std::shared_ptr<CleanupState> state;

    ~Cleanup()
    {
        if (state) {
            state->gate.block();
            ++state->finished;
        }
    }
};

void install_cleanup(const std::shared_ptr<CleanupState> &state)
{
    thread_local Cleanup cleanup;
    cleanup.state = state;
}

void shutdown(bool retired)
{
    const unsigned workers = retired ? 2 : 1;
    auto state = std::make_shared<CleanupState>();
    std::unique_ptr<progschj::ThreadPool> pool(new progschj::ThreadPool(workers));
    auto task_gate = std::make_shared<Gate>();
    std::vector<std::future<void> > tasks;
    for (unsigned i = 0; i < workers; ++i) {
        tasks.push_back(pool->enqueue([state, task_gate] {
            install_cleanup(state);
            task_gate->block();
        }));
    }
    task_gate->wait_entered(workers);
    task_gate->release();
    finish(tasks);
    if (retired) {
        pool->set_pool_size(1);
        state->gate.wait_entered(); // One retired worker is still in TLS cleanup.
    }
    auto destroying = std::async(std::launch::async, [&] { pool.reset(); });
    state->gate.wait_entered(workers);
    require_blocked(destroying, "destruction returned before worker TLS cleanup");
    state->gate.release();
    get_ready(destroying);
    require(state->finished == workers, "destruction missed worker TLS cleanup");
}

} // namespace

int main(int argc, char **argv)
{
    require(argc <= 2, "expected at most one test name");
    const struct Test {
        const char *name;
        void (*run)();
    } tests[] = {
        {"results", results},
        {"zero", zero},
        {"resize", resize},
        {"busy-resize", busy_resize},
        {"worker-resize", worker_resize},
        {"concurrent", concurrent},
        {"drain", drain},
        {"queue-full", queue_full},
        {"enqueue-block", [] { enqueue_block(false); }},
        {"enqueue-block-growth", [] { enqueue_block(true); }},
        {"wait-empty", wait_empty},
        {"wait-idle", wait_idle},
        {"shutdown-active", [] { shutdown(false); }},
        {"shutdown-retired", [] { shutdown(true); }}
    };
    bool ran = false;
    for (const auto &test : tests) {
        if (argc > 1 && std::strcmp(argv[1], test.name) != 0)
            continue;
        ran = true;
        test.run();
        std::printf("PASS: %s\n", test.name);
    }
    require(ran, "unknown test name");
}
