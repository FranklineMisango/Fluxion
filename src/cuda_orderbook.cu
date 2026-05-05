#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gpu_utils.hpp"
#include "orderbook.hpp"

__inline__ __device__ float warpReduceMax(float val) {
    for (int offset = 16; offset > 0; offset /= 2) {
        val = fmaxf(val, __shfl_down_sync(0xffffffff, val, offset));
    }
    return val;
}

__inline__ __device__ float warpReduceMin(float val) {
    for (int offset = 16; offset > 0; offset /= 2) {
        val = fminf(val, __shfl_down_sync(0xffffffff, val, offset));
    }
    return val;
}

__global__ void orderbook_kernel(const float* bids, const float* asks,
                                 const float* volumes, int N,
                                 float* bestBid, float* bestAsk, float* vwap) {
    float localBid = -1e9f;
    float localAsk = 1e9f;
    float num = 0.0f;
    float den = 0.0f;

    for (int i = threadIdx.x; i < N; i += blockDim.x) {
        localBid = fmaxf(localBid, bids[i]);
        localAsk = fminf(localAsk, asks[i]);
        num += bids[i] * volumes[i];
        den += volumes[i];
    }

    localBid = warpReduceMax(localBid);
    localAsk = warpReduceMin(localAsk);

    for (int offset = 16; offset > 0; offset /= 2) {
        num += __shfl_down_sync(0xffffffff, num, offset);
        den += __shfl_down_sync(0xffffffff, den, offset);
    }

    if (threadIdx.x == 0) {
        *bestBid = localBid;
        *bestAsk = localAsk;
        *vwap = num / den;
    }
}

void run_orderbook(const char* path) {
    std::vector<float> bids;
    std::vector<float> asks;
    std::vector<float> volumes;

    std::ifstream file(path);
    std::string line;

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        float b = 0.0f;
        float a = 0.0f;
        float v = 0.0f;

        if (ss >> b >> a >> v) {
            bids.push_back(b);
            asks.push_back(a);
            volumes.push_back(v);
        }
    }

    const int N = static_cast<int>(bids.size());
    if (N == 0) {
        std::fprintf(stderr, "No order book rows loaded from %s\n", path);
        return;
    }

    float* d_bids = nullptr;
    float* d_asks = nullptr;
    float* d_volumes = nullptr;
    float* d_bestBid = nullptr;
    float* d_bestAsk = nullptr;
    float* d_vwap = nullptr;

    gpuCheck(cudaMalloc(&d_bids, N * sizeof(float)));
    gpuCheck(cudaMalloc(&d_asks, N * sizeof(float)));
    gpuCheck(cudaMalloc(&d_volumes, N * sizeof(float)));
    gpuCheck(cudaMalloc(&d_bestBid, sizeof(float)));
    gpuCheck(cudaMalloc(&d_bestAsk, sizeof(float)));
    gpuCheck(cudaMalloc(&d_vwap, sizeof(float)));

    gpuCheck(cudaMemcpy(d_bids, bids.data(), N * sizeof(float), cudaMemcpyHostToDevice));
    gpuCheck(cudaMemcpy(d_asks, asks.data(), N * sizeof(float), cudaMemcpyHostToDevice));
    gpuCheck(cudaMemcpy(d_volumes, volumes.data(), N * sizeof(float), cudaMemcpyHostToDevice));

    orderbook_kernel<<<1, 32>>>(d_bids, d_asks, d_volumes, N,
                                d_bestBid, d_bestAsk, d_vwap);
    gpuCheck(cudaGetLastError());
    gpuCheck(cudaDeviceSynchronize());

    float bestBid = 0.0f;
    float bestAsk = 0.0f;
    float vwap = 0.0f;
    gpuCheck(cudaMemcpy(&bestBid, d_bestBid, sizeof(float), cudaMemcpyDeviceToHost));
    gpuCheck(cudaMemcpy(&bestAsk, d_bestAsk, sizeof(float), cudaMemcpyDeviceToHost));
    gpuCheck(cudaMemcpy(&vwap, d_vwap, sizeof(float), cudaMemcpyDeviceToHost));

    std::printf("Best Bid: %.4f\n", bestBid);
    std::printf("Best Ask: %.4f\n", bestAsk);
    std::printf("VWAP: %.4f\n", vwap);

    cudaFree(d_bids);
    cudaFree(d_asks);
    cudaFree(d_volumes);
    cudaFree(d_bestBid);
    cudaFree(d_bestAsk);
    cudaFree(d_vwap);
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "sample_data/orderbook.csv";
    run_orderbook(path);
    return 0;
}