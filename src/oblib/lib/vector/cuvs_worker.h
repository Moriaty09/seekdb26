/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifndef OCEANBASE_LIB_VECTOR_CUVS_WORKER_H_
#define OCEANBASE_LIB_VECTOR_CUVS_WORKER_H_

#include <condition_variable>
#include <mutex>
#include <pthread.h>

namespace oceanbase {
namespace common {
namespace obvsag {

// Synchronous submissions keep caller-owned jobs alive until completion. The
// single persistent pthread retains the large stack required by hipVS, as well
// as its thread-local GPU runtime state. No task or graph survives its index.
class CuvsWorker {
public:
  using Function = void *(*)(void *);

  CuvsWorker()
  {
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) { return; }
    const int rc = pthread_attr_setstacksize(&attributes, 32UL * 1024 * 1024);
    if (rc == 0) {
      ready_ = pthread_create(&thread_, &attributes, &CuvsWorker::entry, this) == 0;
    }
    pthread_attr_destroy(&attributes);
  }

  ~CuvsWorker()
  {
    // As with index destruction, shutdown requires callers to be quiescent.
    if (ready_) {
      {
        std::lock_guard<std::mutex> guard(work_mutex_);
        stop_ = true;
      }
      work_available_.notify_one();
      pthread_join(thread_, nullptr);
    }
  }

  CuvsWorker(const CuvsWorker &) = delete;
  CuvsWorker &operator=(const CuvsWorker &) = delete;

  bool run(Function function, void *argument)
  {
    if (!ready_ || function == nullptr) { return false; }
    if (pthread_equal(pthread_self(), thread_)) {
      function(argument);
      return true;
    }
    std::lock_guard<std::mutex> submit_guard(submit_mutex_);
    std::unique_lock<std::mutex> work_guard(work_mutex_);
    function_ = function;
    argument_ = argument;
    pending_ = true;
    work_available_.notify_one();
    completed_.wait(work_guard, [this] { return !pending_; });
    return succeeded_;
  }

private:
  static void *entry(void *argument)
  {
    static_cast<CuvsWorker *>(argument)->loop();
    return nullptr;
  }

  void loop()
  {
    std::unique_lock<std::mutex> guard(work_mutex_);
    for (;;) {
      work_available_.wait(guard, [this] { return pending_ || stop_; });
      if (stop_) { return; }
      Function function = function_;
      void *argument = argument_;
      guard.unlock();
      bool succeeded = true;
      try {
        function(argument);
      } catch (...) {
        // Release the waiting caller even if a host-side job throws.
        succeeded = false;
      }
      guard.lock();
      succeeded_ = succeeded;
      function_ = nullptr;
      argument_ = nullptr;
      pending_ = false;
      completed_.notify_one();
    }
  }

  pthread_t thread_{};
  bool ready_ = false;
  bool pending_ = false;
  bool stop_ = false;
  bool succeeded_ = false;
  Function function_ = nullptr;
  void *argument_ = nullptr;
  std::mutex submit_mutex_;
  std::mutex work_mutex_;
  std::condition_variable work_available_;
  std::condition_variable completed_;
};

inline bool run_cuvs_job(CuvsWorker::Function function, void *argument)
{
  static CuvsWorker worker;
  return worker.run(function, argument);
}

} // namespace obvsag
} // namespace common
} // namespace oceanbase
#endif
