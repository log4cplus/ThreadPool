#include <future>
#include <thread>
#include <vector>

#include "ThreadPool/ThreadPool.hpp"

#include <benchmark/benchmark.h>

static void thread_pool_bench(benchmark::State& state)
{
  const auto size = state.range(0);

  for (auto _ : state) {
    benchmark::DoNotOptimize(size);

    progschj::ThreadPool pool(size);
    std::vector<std::future<int> > results;

    for (int i = 0; i < size; ++i) {
      results.emplace_back(pool.enqueue([i] { return i * i; }));
    }

    for (auto&& result : results) {
      result.get();
    }

    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * size * sizeof(int));

  // state.SetLabel("OK");
}
BENCHMARK(thread_pool_bench)
    ->Name("thread_pool_bench")
    ->RangeMultiplier(2)
    ->Range(1, 128);
