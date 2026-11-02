// Exercises the real bridge control flow against instrumented host API doubles.
// This does not test a GPU kernel, Recall, GPU timing, or hipVS ABI compatibility.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <map>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <io.h>
#endif
#include "../seekdb_cuvs_bridge.c"

static std::map<void*, size_t> allocations;
static int alloc_calls, param_creates, resources, indexes, index_params, search_params;
static int fail_alloc, fail_params, fail_search, copy_calls, fail_copy_at;
static int graph_views, bad_graph_shape, fail_restore, fail_sync, restore_calls;
static int build_calls;
static std::atomic<int> active_searches{0};

cuvsError_t cuvsResourcesCreate(cuvsResources_t* p) { *p = malloc(1); ++resources; return 0; }
cuvsError_t cuvsResourcesDestroy(cuvsResources_t p) { free(p); --resources; return 0; }
cuvsError_t cuvsStreamSync(cuvsResources_t) {
  if (fail_sync) { --fail_sync; return 1; } return 0;
}
cuvsError_t cuvsStreamGet(cuvsResources_t, cudaStream_t* stream) { *stream = nullptr; return 0; }
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
  ++build_calls;
  idx->n = t->dl_tensor.shape[0]; idx->dim = t->dl_tensor.shape[1]; idx->degree = p->graph_degree;
  const auto* data = static_cast<float*>(t->dl_tensor.data);
  idx->vectors.assign(data, data + idx->n * idx->dim);
  idx->graph.resize(idx->n * idx->degree);
  for (int64_t row = 0; row < idx->n; ++row) for (int64_t col = 0; col < idx->degree; ++col)
    idx->graph[row * idx->degree + col] = unsigned((row + col + 1) % idx->n);
  return 0;
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
int cudaMemcpy2DAsync(void* dst, size_t dpitch, const void* src, size_t spitch,
                      size_t width, size_t height, int, cudaStream_t) {
  if (++copy_calls == fail_copy_at) return 1;
  for (size_t i = 0; i < height; ++i)
    memcpy(static_cast<char*>(dst) + dpitch * i, static_cast<const char*>(src) + spitch * i, width);
  return 0;
}
cuvsError_t cuvsCagraSerialize(cuvsResources_t, const char* path, cuvsCagraIndex_t idx, bool include_data) {
  assert(include_data); FILE* f = fopen(path, "wb"); assert(f);
  const int64_t header[] = {idx->n, idx->dim, idx->degree};
  assert(fwrite(header, sizeof(header), 1, f) == 1);
  assert(fwrite(idx->graph.data(), sizeof(unsigned), idx->graph.size(), f) == idx->graph.size());
  assert(fwrite(idx->vectors.data(), sizeof(float), idx->vectors.size(), f) == idx->vectors.size());
  fclose(f); return 0;
}
cuvsError_t cuvsCagraDeserialize(cuvsResources_t, const char* path, cuvsCagraIndex_t idx) {
  FILE* f = nullptr;
#ifdef _WIN32
  // Emulate Linux /proc/self/fd reopening for the legacy adaptor wire-code test.
  // Production still uses the kernel procfs; this only makes the host double portable.
  const char prefix[] = "/proc/self/fd/";
  if (strncmp(path, prefix, sizeof(prefix) - 1) == 0) {
    const int descriptor = _dup(atoi(path + sizeof(prefix) - 1));
    if (descriptor >= 0) { f = _fdopen(descriptor, "rb"); if (!f) _close(descriptor); }
    if (f) rewind(f);
  } else
#endif
  { f = fopen(path, "rb"); }
  if (!f) return 1;
  int64_t header[3] = {};
  bool ok = fread(header, sizeof(header), 1, f) == 1;
  if (ok) {
    idx->n = header[0]; idx->dim = header[1]; idx->degree = header[2];
    idx->graph.resize(idx->n * idx->degree); idx->vectors.resize(idx->n * idx->dim);
    ok = fread(idx->graph.data(), sizeof(unsigned), idx->graph.size(), f) == idx->graph.size() &&
         fread(idx->vectors.data(), sizeof(float), idx->vectors.size(), f) == idx->vectors.size();
  }
  fclose(f); return ok ? 0 : 1;
}
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t idx, int64_t* n) { *n = idx->n; return 0; }
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t idx, int64_t* d) { *d = idx->dim; return 0; }
cuvsError_t cuvsCagraIndexGetGraphDegree(cuvsCagraIndex_t idx, int64_t* d) { *d = idx->degree; return 0; }
struct GraphView {
  int64_t shape[2], strides[2]; std::vector<unsigned> padded;
};
cuvsError_t cuvsCagraIndexGetGraph(cuvsCagraIndex_t idx, DLManagedTensor* tensor) {
  auto* view = new GraphView{{idx->n, idx->degree + bad_graph_shape}, {idx->degree + 3, 1}, {}};
  view->padded.resize(2 + idx->n * view->strides[0], ~0u);
  for (int64_t i = 0; i < idx->n; ++i)
    std::copy_n(idx->graph.data() + i * idx->degree, idx->degree,
                view->padded.data() + 2 + i * view->strides[0]);
  *tensor = {};
  tensor->dl_tensor.data = view->padded.data(); tensor->dl_tensor.byte_offset = 2 * sizeof(unsigned);
  tensor->dl_tensor.device.device_type = kDLCUDA; tensor->dl_tensor.ndim = 2;
  tensor->dl_tensor.dtype = {kDLUInt, 32, 1};
  tensor->dl_tensor.shape = view->shape; tensor->dl_tensor.strides = view->strides;
  tensor->manager_ctx = view; ++graph_views;
  tensor->deleter = [](DLManagedTensor* t) { delete static_cast<GraphView*>(t->manager_ctx); --graph_views; };
  return 0;
}
cuvsError_t cuvsCagraIndexFromArgs(cuvsResources_t, int metric, DLManagedTensor* graph,
                                  DLManagedTensor* dataset, cuvsCagraIndex_t idx) {
  ++restore_calls;
  if (fail_restore) { --fail_restore; return 1; }
  assert(metric == 0 && graph->dl_tensor.device.device_type == kDLCPU &&
         dataset->dl_tensor.device.device_type == kDLCPU);
  assert(graph->dl_tensor.dtype.code == kDLUInt && graph->dl_tensor.dtype.bits == 32);
  assert(dataset->dl_tensor.dtype.code == kDLFloat && dataset->dl_tensor.dtype.bits == 32);
  idx->n = dataset->dl_tensor.shape[0]; idx->dim = dataset->dl_tensor.shape[1];
  idx->degree = graph->dl_tensor.shape[1];
  assert(graph->dl_tensor.shape[0] == idx->n);
  const auto* g = static_cast<unsigned*>(graph->dl_tensor.data);
  const auto* v = static_cast<float*>(dataset->dl_tensor.data);
  idx->graph.assign(g, g + idx->n * idx->degree);
  idx->vectors.assign(v, v + idx->n * idx->dim);
  return 0;
}

#ifdef SEEKDB_TEST_ADAPTOR_SNAPSHOT
#include "test_adaptor_snapshot.h"
#endif

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
  assert(sk_cuvs_cpu_budget(128, 1600000, 100000) == 16);
  assert(sk_cuvs_cpu_budget(128, 2400000, 100000) == 24);
  assert(sk_cuvs_cpu_budget(4, 2400000, 100000) == 4);
  assert(sk_cuvs_cpu_budget(128, 0, 0) == 24);
  assert(sk_cuvs_cpu_budget(128, 50000, 100000) == 1);
  assert(sk_cuvs_cpu_budget(0, 0, 0) == 1);
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
  h = seekdb_cuvs_build(base.data(), 2000, 128); assert(h);
  long degree = 0;
  assert(seekdb_cuvs_graph_degree(h, &degree) == 0 && degree == 128);
  std::vector<unsigned> graph(2000 * degree);
  bad_graph_shape = 1;
  assert(seekdb_cuvs_export_graph(h, graph.data(), degree) != 0 && graph_views == 0);
  bad_graph_shape = 0;
  fail_copy_at = copy_calls + 1;
  assert(seekdb_cuvs_export_graph(h, graph.data(), degree) != 0 && graph_views == 0);
  assert(seekdb_cuvs_export_graph(h, graph.data(), degree) == 0 && graph_views == 0);
  const auto expected = static_cast<sk_cuvs_index*>(h)->idx->graph;
  assert(graph == expected); seekdb_cuvs_free(h);
  fail_restore = 1;
  assert(!seekdb_cuvs_restore_graph(base.data(), 2000, 128, graph.data(), degree));
  fail_sync = 1;
  assert(!seekdb_cuvs_restore_graph(base.data(), 2000, 128, graph.data(), degree));
  h = seekdb_cuvs_restore_graph(base.data(), 2000, 128, graph.data(), degree); assert(h);
  graph[0] = 2000; // no alias of caller storage; invalid row offset rejected on another restore
  assert(static_cast<sk_cuvs_index*>(h)->idx->graph == expected);
  assert(!seekdb_cuvs_restore_graph(base.data(), 2000, 128, graph.data(), degree));
  search(h, 100); seekdb_cuvs_free(h);
#ifdef SEEKDB_TEST_ADAPTOR_SNAPSHOT
  test_adaptor_snapshot(argv[1]);
#endif
  assert(allocations.empty() && resources == 0 && indexes == 0 && index_params == 0 &&
         search_params == 0 && graph_views == 0);
  puts("PASS host workspace lifecycle, parameter equivalence, errors, concurrency, reload; GPU validation pending");
}
