// Compile the actual adaptor snapshot/registry implementation with host API
// doubles. The runner extracts that section verbatim; no alternate wire codec.
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unistd.h>
#include "../../../src/oblib/lib/vector/cuvs_worker.h"

namespace vsag {
struct Allocator {
  void* Allocate(size_t bytes) { return malloc(bytes); }
  void Deallocate(void* p) { free(p); }
};
struct FilterInterface { bool test(int64_t) { return false; } };
}
struct ObTimeUtility {
  static int64_t current_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  }
};
template <typename... T> void test_log(const T&...) {}
#define LOG_WARN(...) test_log(__VA_ARGS__)
#define LOG_INFO(...) test_log(__VA_ARGS__)
#define K(x) (x)
#define KP(x) (x)
#define OB_SUCCESS 0
#define OB_ERR_UNEXPECTED -1
#define OB_ERR_VSAG_RETURN_ERROR -2
#define OB_SUCC(x) ((x) == OB_SUCCESS)
using oceanbase::common::obvsag::run_cuvs_job;
using vsag::FilterInterface;
#include "adaptor_snapshot_under_test.h"

static void test_adaptor_snapshot(const char* legacy_file)
{
  constexpr int n = 512;
  for (int dim : {128, 256}) {
    std::vector<float> vectors(n * dim);
    for (size_t i = 0; i < vectors.size(); ++i) vectors[i] = float(int(i % 233) - 116) / 8;
    std::vector<int64_t> vids(n);
    for (int i = 0; i < n; ++i) vids[i] = (i % 5 ? 1 : -1) * (int64_t(i) * 17 + 1009);
    int source_key = 0, loaded_key = 0, bad_key = 0;
    ob_cuvs_add(&source_key, vectors.data(), vids.data(), dim, n);
    const int builds_before = build_calls;
    int ret = -1;
    std::ostringstream output(std::ios::binary);
    assert(ob_cuvs_serialize(&source_key, output, ret) && ret == OB_SUCCESS);
    assert(build_calls == builds_before + 1);
    const auto snapshot = output.str();
    assert(snapshot.substr(0, 8) == "OBCUVS03");
    const size_t vector_offset = 32 + n * sizeof(int64_t);
    const size_t graph_offset = vector_offset + vectors.size() * sizeof(float);
    assert(snapshot.size() == graph_offset + n * 128 * sizeof(unsigned));
    assert(memcmp(snapshot.data() + 32, vids.data(), vids.size() * sizeof(int64_t)) == 0);
    assert(memcmp(snapshot.data() + vector_offset, vectors.data(), vectors.size() * sizeof(float)) == 0);
    auto* original = static_cast<sk_cuvs_index*>(g_ob_cuvs_reg.at(&source_key)->bridge_.get());
    assert(memcmp(snapshot.data() + graph_offset, original->idx->graph.data(), n * 128 * sizeof(unsigned)) == 0);
    std::istringstream input(snapshot.substr(8), std::ios::binary);
    assert(ob_cuvs_deserialize(&loaded_key, dim, input, true) == OB_SUCCESS);
    auto* loaded = static_cast<sk_cuvs_index*>(g_ob_cuvs_reg.at(&loaded_key)->bridge_.get());
    assert(loaded->idx->graph == original->idx->graph && loaded->idx->vectors == vectors);
    assert(g_ob_cuvs_reg.at(&loaded_key)->buf_ids_ == vids);
    assert(build_calls == builds_before + 1); // restore never builds a graph again
    std::vector<float> query(dim, 3);
    const float* distances = nullptr;
    const int64_t* ids = nullptr;
    int64_t result_size = 0;
    assert(ob_cuvs_try_search(&loaded_key, nullptr, query.data(), dim, 100,
                               distances, ids, result_size, nullptr, false));
    assert(result_size == 100);
    for (int i = 0; i < 100; ++i) assert(ids[i] == vids[3 + i] && distances[i] == float(i));
    free(const_cast<int64_t*>(ids)); free(const_cast<float*>(distances));
    ob_cuvs_erase(&loaded_key);

    auto reject = [&](const std::string& corrupt, int expected_dim = 0) {
      std::istringstream broken(corrupt.substr(8), std::ios::binary);
      assert(ob_cuvs_deserialize(&bad_key, expected_dim ? expected_dim : dim, broken, true) != OB_SUCCESS);
      assert(g_ob_cuvs_reg.count(&bad_key) == 0);
    };
    for (size_t cut : {size_t(8), size_t(31), vector_offset - 1, graph_offset - 1, snapshot.size() - 1})
      reject(snapshot.substr(0, cut));
    reject(snapshot, dim + 1);
    auto corrupt = snapshot;
    const uint64_t wrong_bytes = 1;
    memcpy(&corrupt[8], &wrong_bytes, sizeof(wrong_bytes)); reject(corrupt);
    corrupt = snapshot;
    const int32_t invalid_degree = 0;
    memcpy(&corrupt[20], &invalid_degree, sizeof(invalid_degree)); reject(corrupt);
    corrupt = snapshot;
    const unsigned invalid_neighbor = n;
    memcpy(&corrupt[graph_offset], &invalid_neighbor, sizeof(invalid_neighbor)); reject(corrupt);
    fail_restore = 1; reject(snapshot);

    // Construct the unchanged V5 wire layout with an actual legacy bridge file.
    assert(seekdb_cuvs_serialize(original, legacy_file) == 0);
    std::ifstream file(legacy_file, std::ios::binary);
    const std::string legacy_graph((std::istreambuf_iterator<char>(file)), {});
    std::ostringstream legacy(std::ios::binary);
    const uint64_t graph_bytes = legacy_graph.size();
    const int32_t dim32 = dim, reserved = 0;
    const int64_t n64 = n;
    assert(ob_cuvs_write(legacy, &graph_bytes, sizeof(graph_bytes)) &&
           ob_cuvs_write(legacy, &dim32, sizeof(dim32)) &&
           ob_cuvs_write(legacy, &reserved, sizeof(reserved)) &&
           ob_cuvs_write(legacy, &n64, sizeof(n64)) &&
           ob_cuvs_write(legacy, vids.data(), vids.size() * sizeof(int64_t)) &&
           ob_cuvs_write(legacy, vectors.data(), vectors.size() * sizeof(float)) &&
           ob_cuvs_write(legacy, legacy_graph.data(), legacy_graph.size()));
    std::istringstream old_input(legacy.str(), std::ios::binary);
    assert(ob_cuvs_deserialize(&loaded_key, dim, old_input, false) == OB_SUCCESS);
    loaded = static_cast<sk_cuvs_index*>(g_ob_cuvs_reg.at(&loaded_key)->bridge_.get());
    assert(loaded->idx->graph == original->idx->graph && loaded->idx->vectors == vectors);
    ob_cuvs_erase(&loaded_key);
    ob_cuvs_erase(&source_key);
  }
  assert(g_ob_cuvs_reg.empty() && g_ob_cuvs_marked.empty());
  puts("PASS actual adaptor: compact snapshot byte identity, IDs, restore without rebuild, truncation, corrupt graph, legacy layout");
}
