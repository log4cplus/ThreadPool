#include <future>
#include <thread>
#include <vector>

#include "ThreadPool/ThreadPool.hpp"

#include "gtest/gtest.h"

TEST(threadpool, onethread)
{
  progschj::ThreadPool pool(1);

  // enqueue and store future
  auto result = pool.enqueue([](int answer) { return answer; }, 42);

  // get result from future
  EXPECT_EQ(42, result.get());
}

TEST(threadpool, twothread)
{
  progschj::ThreadPool pool(2);

  // enqueue and store future
  auto result = pool.enqueue([](int answer) { return answer; }, 128);

  // get result from future
  EXPECT_EQ(128, result.get());
}

TEST(threadpool, manythread)
{
  progschj::ThreadPool pool(128);

  // enqueue and store future
  auto result = pool.enqueue([](int answer) { return answer; }, 256);

  // get result from future
  EXPECT_EQ(256, result.get());
}

TEST(threadpool, manyworker)
{
  progschj::ThreadPool pool;
  std::vector<std::future<int> > results;

  for (int i = 0; i < 2048; ++i) {
    results.emplace_back(pool.enqueue([i] { return i * i; }));
  }

  for (int i = 0; i < 2048; ++i) {
    EXPECT_EQ(i * i, results[i].get());
  }
}
