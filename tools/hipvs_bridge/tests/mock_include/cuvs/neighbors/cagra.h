#pragma once
#include <cuvs/core/c_api.h>
#include <dlpack/dlpack.h>
enum { NN_DESCENT = 1, NO_FILTER = 0 };
typedef struct { int build_algo, graph_degree, intermediate_graph_degree, nn_descent_niter; } MockIndexParams;
typedef MockIndexParams* cuvsCagraIndexParams_t;
typedef struct { int64_t n, dim; } MockIndex;
typedef MockIndex* cuvsCagraIndex_t;
typedef struct { size_t itopk_size, search_width; } MockSearchParams;
typedef MockSearchParams* cuvsCagraSearchParams_t;
typedef struct { int type; uintptr_t addr; } cuvsFilter;
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t*);
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t);
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t*);
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t);
cuvsError_t cuvsCagraBuild(cuvsResources_t, cuvsCagraIndexParams_t, DLManagedTensor*, cuvsCagraIndex_t);
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t*);
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t);
cuvsError_t cuvsCagraSearch(cuvsResources_t, cuvsCagraSearchParams_t, cuvsCagraIndex_t, DLManagedTensor*, DLManagedTensor*, DLManagedTensor*, cuvsFilter);
cuvsError_t cuvsCagraSerialize(cuvsResources_t, const char*, cuvsCagraIndex_t, bool);
cuvsError_t cuvsCagraDeserialize(cuvsResources_t, const char*, cuvsCagraIndex_t);
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t, int64_t*);
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t, int64_t*);
