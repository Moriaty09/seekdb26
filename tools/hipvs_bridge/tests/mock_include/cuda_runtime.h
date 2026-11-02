#pragma once
#include <stddef.h>
enum { cudaSuccess = 0, cudaMemcpyDefault = 0 };
int cudaMemcpy(void*, const void*, size_t, int);
typedef void* cudaStream_t;
int cudaMemcpy2DAsync(void*, size_t, const void*, size_t, size_t, size_t, int, cudaStream_t);
