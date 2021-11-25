#include <future>
#include <iostream>
#include <vector>

#include "ThreadPool/ThreadPool.hpp"

struct Processor
{
  auto operator()(int value, int multi) -> int
  {
    return value * multi;
  }
};

auto main() -> int
{
  // create thread pool
  progschj::ThreadPool pool(std::thread::hardware_concurrency());

  std::vector<std::future<int>> results {};

  // enqueue and store future
  for (int i = 0; i < 8; ++i) {
    results.emplace_back(pool.enqueue(Processor(), i, i * 2));
  }

  // get result from future
  for (auto&& result : results) {
    std::cout << result.get() << std::endl;
  }
}
