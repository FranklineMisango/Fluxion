#include <cstdio>

#include <curand.h>
#include <curand_kernel.h>

#include "gpu_utils.hpp"
#include "monte_carlo.hpp"

__global__ void mc_kernel(float* results, int paths, float S0, float mu, float sigma, float dt) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= paths) {
        return;
    }

    curandState state;
    curand_init(1234ULL, idx, 0, &state);

    float S = S0;
    float z = curand_normal(&state);
    S = S * expf((mu - 0.5f * sigma * sigma) * dt + sigma * sqrtf(dt) * z);

    results[idx] = S;
}

void run_monte_carlo() {
    const int paths = 1 << 20;
    float* d_results = nullptr;
    float* h_results = new float[paths];

    gpuCheck(cudaMalloc(&d_results, paths * sizeof(float)));

    mc_kernel<<<(paths + 255) / 256, 256>>>(d_results, paths, 100.0f, 0.05f, 0.2f, 1.0f);
    gpuCheck(cudaGetLastError());
    gpuCheck(cudaDeviceSynchronize());
    gpuCheck(cudaMemcpy(h_results, d_results, paths * sizeof(float), cudaMemcpyDeviceToHost));

    float sum = 0.0f;
    for (int i = 0; i < paths; i++) {
        sum += h_results[i];
    }

    std::printf("Mean terminal price: %.4f\n", sum / paths);

    cudaFree(d_results);
    delete[] h_results;
}

int main() {
    run_monte_carlo();
    return 0;
}