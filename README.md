ThreadPool
==========

A simple C++11 Thread Pool implementation.

Build and run the cross-platform regression suite with CMake and CTest; see
[the test documentation](tests/README.md) for commands and the CI compiler matrix.

Basic usage:
```c++
// create thread pool with 4 worker threads
ThreadPool pool(4);

// enqueue and store future
auto result = pool.enqueue([](int answer) { return answer; }, 42);

// get result from future
std::cout << result.get() << std::endl;

```
