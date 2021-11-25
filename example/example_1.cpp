#include <iostream>

#include "ThreadPool/ThreadPool.hpp"

auto main() -> int
{
  // create thread pool with 4 worker threads
  progschj::ThreadPool pool(4);

  // enqueue and store future
  auto result = pool.enqueue([](int answer) { return answer; }, 42);

  // get result from future
  std::cout << result.get() << std::endl;
}
