#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../host_thread_budget.h"

static int calls;
static std::string configured;
extern "C" void kmp_set_defaults(const char* settings) { ++calls; configured = settings; }

int main()
{
  sk_cuvs_configure_host_threads();
  sk_cuvs_configure_host_threads();
  const bool explicit_threads = getenv("OMP_NUM_THREADS") != nullptr;
  const bool explicit_blocktime = getenv("KMP_BLOCKTIME") != nullptr;
  assert(calls == (explicit_threads && explicit_blocktime ? 0 : 1));
  assert((configured.find("OMP_NUM_THREADS=") != std::string::npos) == !explicit_threads);
  assert((configured.find("KMP_BLOCKTIME=0") != std::string::npos) == !explicit_blocktime);
  if (!explicit_threads) {
    const int threads = std::stoi(configured.substr(std::string("OMP_NUM_THREADS=").size()));
    assert(threads >= 1 && threads <= 16);
  }
  std::puts("PASS OpenMP budget: one-time setup and explicit environment preferences");
}
