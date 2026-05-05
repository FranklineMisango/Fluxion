#pragma once

#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>

inline void gpuAssert(cudaError_t code, const char* file, int line) {
    if (code != cudaSuccess) {
        std::fprintf(stderr, "CUDA Error: %s %s:%d\n",
                     cudaGetErrorString(code), file, line);
        std::exit(code);
    }
}

#define gpuCheck(ans) { gpuAssert((ans), __FILE__, __LINE__); }