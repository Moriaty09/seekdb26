#include "../../../src/oblib/lib/vector/cuvs_worker.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using oceanbase::common::obvsag::CuvsWorker;

struct State {
  pthread_t worker{};
  bool identified = false;
  int calls = 0;
};
struct Job { State *state; int result = 0; };

static void *execute(void *argument)
{
  Job &job = *static_cast<Job *>(argument);
  if (!job.state->identified) {
    job.state->worker = pthread_self();
    job.state->identified = true;
#ifdef __linux__
    pthread_attr_t attributes;
    assert(pthread_getattr_np(pthread_self(), &attributes) == 0);
    size_t stack_size = 0;
    assert(pthread_attr_getstacksize(&attributes, &stack_size) == 0);
    assert(stack_size >= 32UL * 1024 * 1024);
    pthread_attr_destroy(&attributes);
#endif
  }
  assert(pthread_equal(job.state->worker, pthread_self()));
  ++job.state->calls;
  job.result = 42;
  return nullptr;
}

static void *throws(void *) { throw std::runtime_error("test job error"); }

struct Nested { CuvsWorker *worker; Job *job; };
static void *nested(void *argument)
{
  Nested &job = *static_cast<Nested *>(argument);
  assert(job.worker->run(execute, job.job));
  return nullptr;
}

int main()
{
  State state;
  {
    CuvsWorker worker;
    assert(!worker.run(nullptr, nullptr));
    std::vector<std::thread> clients;
    for (int client = 0; client < 8; ++client) {
      clients.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
          Job job{&state};
          assert(worker.run(execute, &job));
          assert(job.result == 42);
        }
      });
    }
    for (auto &client : clients) { client.join(); }
    assert(state.calls == 800);
    assert(!worker.run(throws, nullptr));
    Job job{&state};
    Nested recursive{&worker, &job};
    assert(worker.run(nested, &recursive));
    assert(job.result == 42 && state.calls == 801);
  }
  std::puts("PASS persistent GPU worker: one large-stack thread, 800 concurrent submissions, exception recovery, recursion, shutdown");
}
