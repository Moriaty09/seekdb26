/* Bound overlapping NN-descent OpenMP teams to the process CPU allocation.
 * Uses the already-loaded LLVM OpenMP public API; no hipVS implementation is changed. */
#ifndef SEEKDB_CUVS_HOST_THREAD_BUDGET_H
#define SEEKDB_CUVS_HOST_THREAD_BUDGET_H

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

static unsigned sk_cuvs_cpu_budget(unsigned affinity, unsigned long long quota,
                                   unsigned long long period)
{
  unsigned budget = affinity ? affinity : 1;
  if (budget > 24) budget = 24;
  if (period && quota) {
    unsigned long long limited = quota / period;
    if (!limited) limited = 1;
    if (limited < budget) budget = (unsigned)limited;
  }
  return budget;
}

#ifdef __linux__
#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>

static void sk_cuvs_init_host_threads(void)
{
  typedef void (*SetDefaults)(const char*);
  SetDefaults configure = (SetDefaults)dlsym(RTLD_DEFAULT, "kmp_set_defaults");
  if (!configure) return; // Other OpenMP runtimes keep their existing behavior.
  long online = sysconf(_SC_NPROCESSORS_ONLN);
  unsigned affinity = online > 0 ? (unsigned)online : 1;
#ifdef CPU_SETSIZE
  cpu_set_t allowed;
  if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0 && CPU_COUNT(&allowed) > 0)
    affinity = (unsigned)CPU_COUNT(&allowed);
#endif
  unsigned long long quota = 0, period = 0;
  FILE* limit = fopen("/sys/fs/cgroup/cpu.max", "r");
  if (limit) {
    if (fscanf(limit, "%llu %llu", &quota, &period) != 2) { quota = 0; period = 0; }
    fclose(limit);
  } else {
    limit = fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r");
    if (limit) { if (fscanf(limit, "%llu", &quota) != 1) quota = 0; fclose(limit); }
    limit = fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r");
    if (limit) { if (fscanf(limit, "%llu", &period) != 1) period = 0; fclose(limit); }
  }
  const unsigned budget = sk_cuvs_cpu_budget(affinity, quota, period);
  // Passive teams sleep while GPU kernels run. Cap the host team at the tested
  // 16-thread size, and shrink it further for smaller CPU allocations.
  const unsigned threads = budget < 16 ? budget : 16;
  char settings[96] = {};
  const int explicit_threads = getenv("OMP_NUM_THREADS") != NULL;
  const int explicit_blocktime = getenv("KMP_BLOCKTIME") != NULL;
  if (!explicit_threads && !explicit_blocktime) {
    snprintf(settings, sizeof(settings), "OMP_NUM_THREADS=%u|KMP_BLOCKTIME=0", threads);
  } else if (!explicit_threads) {
    snprintf(settings, sizeof(settings), "OMP_NUM_THREADS=%u", threads);
  } else if (!explicit_blocktime) {
    snprintf(settings, sizeof(settings), "KMP_BLOCKTIME=0");
  }
  if (settings[0]) {
    configure(settings);
    fprintf(stderr, "[seekdb_cuvs] OpenMP cpu_budget=%u settings=%s\n", budget, settings);
  }
}

static void sk_cuvs_configure_host_threads(void)
{
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, sk_cuvs_init_host_threads);
}
#else
static void sk_cuvs_configure_host_threads(void) {}
#endif
#endif
