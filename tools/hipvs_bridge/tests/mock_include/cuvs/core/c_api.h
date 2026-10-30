#pragma once
// Host-only test double. Never add this directory to a production build.
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef void* cuvsResources_t;
typedef int cuvsError_t;
#define CUVS_SUCCESS 0
cuvsError_t cuvsResourcesCreate(cuvsResources_t*);
cuvsError_t cuvsResourcesDestroy(cuvsResources_t);
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void**, size_t);
cuvsError_t cuvsRMMFree(cuvsResources_t, void*, size_t);
const char* cuvsGetLastErrorText(void);
