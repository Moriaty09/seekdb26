#pragma once
#include <stddef.h>
enum { cudaSuccess = 0, cudaMemcpyDefault = 0 };
int cudaMemcpy(void*, const void*, size_t, int);
