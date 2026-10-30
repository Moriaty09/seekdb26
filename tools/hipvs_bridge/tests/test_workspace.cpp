// Exercises the real bridge control flow against instrumented host API doubles.
// This does not test a GPU kernel, Recall, GPU timing, or hipVS ABI compatibility.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <map>
#include <thread>
#include <vector>
#include "../seekdb_cuvs_bridge.c"

static std::map<void*, size_t> allocations;
static int alloc_calls, param_creates, resources, indexes, index_params, search_params;
static int fail_alloc, fail_params, fail_search, copy_calls, fail_copy_at;
static std::atomic<int> active_searches{0};

cuvsError_t cuvsResourcesCreate(cuvsResources_t* p) { *p = malloc(1); ++resources; return 0; }
cuvsError_t cuvsResourcesDestroy(cuvsResources_t p) { free(p); --resources; return 0; }
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void** p, size_t n) {
  if (fail_alloc) { --fail_alloc; return 1; }
  *p = malloc(n); assert(*p); allocations[*p] = n; ++alloc_calls; return 0;
}
cuvsError_t cuvsRMMFree(cuvsResources_t, void* p, size_t n) {
  assert(allocations.count(p) && allocations[p] == n);
  allocations.erase(p); free(p); return 0;
}
const char* cuvsGetLastErrorText() { return "injected host-test failure"; }
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t* p) { *p = new MockIndexParams{}; ++index_params; return 0; }
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t p) { delete p; --index_params; return 0; }
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t* p) { *p = new MockIndex{}; ++indexes; return 0; }
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t p) { delete p; --indexes; return 0; }
cuvsError_t cuvsCagraBuild(cuvsResources_t, cuvsCagraIndexParams_t p, DLManagedTensor* t, cuvsCagraIndex_t idx) {
  assert(p->build_algo == NN_DESCENT && p->graph_degree == 128 &&
         p->intermediate_graph_degree == 256 && p->nn_descent_niter == 100);
  idx->n = t->dl_tensor.shape[0]; idx->dim = t->dl_tensor.shape[1]; return 0;
}
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t* p) {
  if (fail_params) { --fail_params; return 1; }
  *p = new MockSearchParams{64, 1}; ++param_creates; ++search_params; return 0;
}
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t p) { delete p; --search_params; return 0; }
cuvsError_t cuvsCagraSearch(cuvsResources_t, cuvsCagraSearchParams_t p, cuvsCagraIndex_t,
                          DLManagedTensor* q, DLManagedTensor* n, DLManagedTensor* d, cuvsFilter) {
  assert(active_searches.fetch_add(1) == 0);
  const size_t topk = n->dl_tensor.shape[1];
  assert(p->itopk_size == std::max(size_t(512), topk * 4) && p->search_width == 4);
  if (fail_search) { --fail_search; --active_searches; return 1; }
  const int offset = int(static_cast<float*>(q->dl_tensor.data)[0]);
  for (int64_t row = 0; row < n->dl_tensor.shape[0]; ++row) {
    for (size_t k = 0; k < topk; ++k) {
      static_cast<unsigned*>(n->dl_tensor.data)[row * topk + k] = unsigned(offset + k);
      static_cast<float*>(d->dl_tensor.data)[row * topk + k] = float(k);
    }
  }
  std::this_thread::yield(); --active_searches; return 0;
}
int cudaMemcpy(void* dst, const void* src, size_t bytes, int) {
  if (++copy_calls == fail_copy_at) return 1;
  memcpy(dst, src, bytes); return 0;
}
cuvsError_t cuvsCagraSerialize(cuvsResources_t, const char* path, cuvsCagraIndex_t idx, bool include_data) {
  assert(include_data); FILE* f = fopen(path, "wb"); assert(f);
  assert(fwrite(idx, sizeof(*idx), 1, f) == 1); fclose(f); return 0;
}
cuvsError_t cuvsCagraDeserialize(cuvsResources_t, const char* path, cuvsCagraIndex_t idx) {
  FILE* f = fopen(path, "rb"); if (!f) return 1;
  const bool ok = fread(idx, sizeof(*idx), 1, f) == 1; fclose(f); return ok ? 0 : 1;
}
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t idx, int64_t* n) { *n = idx->n; return 0; }
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t idx, int64_t* d) { *d = idx->dim; return 0; }

static void search(void* h, int k, int nq = 1) {
  std::vector<float> q(nq * 128, 7), distances(nq * k, -1);
  std::vector<unsigned> ids(nq * k, ~0u);
  assert(seekdb_cuvs_search(h, q.data(), nq, k, ids.data(), distances.data()) == 0);
  for (int row = 0; row < nq; ++row) for (int i = 0; i < k; ++i) {
    assert(ids[row * k + i] == unsigned(7 + i));
    assert(distances[row * k + i] == float(i));
  }
}

int main(int argc, char** argv) {
  assert(argc == 2);
  std::vector<float> base(2000 * 128);
  assert(!seekdb_cuvs_build(nullptr, 2000, 128));
  void* h = seekdb_cuvs_build(base.data(), 2000, 128); assert(h);
  float query[128] = {}; unsigned ids[2000]; float distances[2000];
  fail_alloc = 1;
  assert(seekdb_cuvs_search(h, query, 1, 10, ids, distances) == 10);
  fail_params = 1;
  assert(seekdb_cuvs_search(h, query, 1, 10, ids, distances) == 14);
  search(h, 10);
  const int warm_allocs = alloc_calls, warm_params = param_creates;
  for (int i = 0; i < 100; ++i) search(h, 10);
  assert(alloc_calls == warm_allocs && param_creates == warm_params);
  search(h, 100); search(h, 300); search(h, 10); // no sticky itopk after large k
  const size_t retained = allocations.size();
  search(h, 100, 5); search(h, 1500); assert(allocations.size() == retained);
  fail_search = 1;
  assert(seekdb_cuvs_search(h, query, 1, 10, ids, distances) == 15); search(h, 10);
  fail_copy_at = copy_calls + 3;
  assert(seekdb_cuvs_search(h, query, 1, 10, ids, distances) == 17); search(h, 10);
  assert(seekdb_cuvs_search(h, query, 1, 2001, ids, distances) != 0);
  assert(seekdb_cuvs_search(h, query, 0, 10, ids, distances) != 0);
  assert(seekdb_cuvs_search(h, nullptr, 1, 10, ids, distances) != 0);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] { for (int j = 0; j < 100; ++j) search(h, 100); });
  for (auto& thread : threads) thread.join();
  assert(seekdb_cuvs_serialize(h, argv[1]) == 0);
  seekdb_cuvs_free(h);
  assert(!seekdb_cuvs_deserialize(argv[1], 1999, 128));
  h = seekdb_cuvs_deserialize(argv[1], 2000, 128); assert(h); search(h, 100); seekdb_cuvs_free(h);
  assert(allocations.empty() && resources == 0 && indexes == 0 && index_params == 0 && search_params == 0);
  puts("PASS host workspace lifecycle, parameter equivalence, errors, concurrency, reload; GPU validation pending");
}
