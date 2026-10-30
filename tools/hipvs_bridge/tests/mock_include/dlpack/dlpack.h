#pragma once
#include <stdint.h>
enum { kDLCPU = 1, kDLCUDA = 2, kDLUInt = 1, kDLFloat = 2 };
typedef struct { int device_type; int device_id; } DLDevice;
typedef struct { uint8_t code; uint8_t bits; uint16_t lanes; } DLDataType;
typedef struct {
  void* data; DLDevice device; int ndim; DLDataType dtype;
  int64_t* shape; int64_t* strides; uint64_t byte_offset;
} DLTensor;
typedef struct { DLTensor dl_tensor; void* manager_ctx; void* deleter; } DLManagedTensor;
