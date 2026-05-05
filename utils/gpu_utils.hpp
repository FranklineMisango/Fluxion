#pragma once
#include <cuda_runtime.h>
#include <cstdio>

inline void gpuAssert(cudaError_t code, const char* file, int line) {
    if (code != cudaSuccess) {
        fprintf(stderr, "CUDA Error: %s %s:%d\n",
                cudaGetErrorString(code), file, line);
        exit(code);
    }
}

#define gpuCheck(ans) { gpuAssert((ans), __FILE__, __LINE__); }