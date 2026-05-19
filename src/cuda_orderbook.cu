#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>

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

void run_orderbook_streaming(std::istream& in, int batchSize = 1024) {
    // ping-pong buffers (2) to overlap H2D copy and kernel execution
    const int nBuffers = 2;

    // allocate pinned host buffers for bids/asks/vols
    float* h_bids[nBuffers];
    float* h_asks[nBuffers];
    float* h_vols[nBuffers];
    for (int i = 0; i < nBuffers; ++i) {
        gpuCheck(cudaHostAlloc(reinterpret_cast<void**>(&h_bids[i]), batchSize * sizeof(float), cudaHostAllocDefault));
        gpuCheck(cudaHostAlloc(reinterpret_cast<void**>(&h_asks[i]), batchSize * sizeof(float), cudaHostAllocDefault));
        gpuCheck(cudaHostAlloc(reinterpret_cast<void**>(&h_vols[i]), batchSize * sizeof(float), cudaHostAllocDefault));
    }

    // device buffers per ping
    float* d_bids[nBuffers];
    float* d_asks[nBuffers];
    float* d_vols[nBuffers];
    float* d_bestBid[nBuffers];
    float* d_bestAsk[nBuffers];
    float* d_vwap[nBuffers];
    for (int i = 0; i < nBuffers; ++i) {
        gpuCheck(cudaMalloc(&d_bids[i], batchSize * sizeof(float)));
        gpuCheck(cudaMalloc(&d_asks[i], batchSize * sizeof(float)));
        gpuCheck(cudaMalloc(&d_vols[i], batchSize * sizeof(float)));
        gpuCheck(cudaMalloc(&d_bestBid[i], sizeof(float)));
        gpuCheck(cudaMalloc(&d_bestAsk[i], sizeof(float)));
        gpuCheck(cudaMalloc(&d_vwap[i], sizeof(float)));
    }

    // pinned host result buffers
    float* h_bestBid[nBuffers];
    float* h_bestAsk[nBuffers];
    float* h_vwap[nBuffers];
    for (int i = 0; i < nBuffers; ++i) {
        gpuCheck(cudaHostAlloc(reinterpret_cast<void**>(&h_bestBid[i]), sizeof(float), cudaHostAllocDefault));
        gpuCheck(cudaHostAlloc(reinterpret_cast<void**>(&h_bestAsk[i]), sizeof(float), cudaHostAllocDefault));
        gpuCheck(cudaHostAlloc(reinterpret_cast<void**>(&h_vwap[i]), sizeof(float), cudaHostAllocDefault));
    }

    // streams
    cudaStream_t streams[nBuffers];
    for (int i = 0; i < nBuffers; ++i) gpuCheck(cudaStreamCreate(&streams[i]));

    int cur = 0;
    int filled = 0;
    std::string line;

    while (std::getline(in, line)) {
        std::stringstream ss(line);
        float b = 0.0f, a = 0.0f, v = 0.0f;
        if (!(ss >> b >> a >> v)) continue;

        h_bids[cur][filled] = b;
        h_asks[cur][filled] = a;
        h_vols[cur][filled] = v;
        ++filled;

        if (filled >= batchSize) {
            const int N = filled;

            // async copy H->D on current stream
            gpuCheck(cudaMemcpyAsync(d_bids[cur], h_bids[cur], N * sizeof(float), cudaMemcpyHostToDevice, streams[cur]));
            gpuCheck(cudaMemcpyAsync(d_asks[cur], h_asks[cur], N * sizeof(float), cudaMemcpyHostToDevice, streams[cur]));
            gpuCheck(cudaMemcpyAsync(d_vols[cur], h_vols[cur], N * sizeof(float), cudaMemcpyHostToDevice, streams[cur]));

            // launch kernel on stream
            orderbook_kernel<<<1, 32, 0, streams[cur]>>>(d_bids[cur], d_asks[cur], d_vols[cur], N,
                                                        d_bestBid[cur], d_bestAsk[cur], d_vwap[cur]);
            gpuCheck(cudaGetLastError());

            // async copy results back
            gpuCheck(cudaMemcpyAsync(h_bestBid[cur], d_bestBid[cur], sizeof(float), cudaMemcpyDeviceToHost, streams[cur]));
            gpuCheck(cudaMemcpyAsync(h_bestAsk[cur], d_bestAsk[cur], sizeof(float), cudaMemcpyDeviceToHost, streams[cur]));
            gpuCheck(cudaMemcpyAsync(h_vwap[cur], d_vwap[cur], sizeof(float), cudaMemcpyDeviceToHost, streams[cur]));

            // wait for this stream to finish before printing and reusing the buffer
            gpuCheck(cudaStreamSynchronize(streams[cur]));

            std::printf("Best Bid: %.4f\n", *h_bestBid[cur]);
            std::printf("Best Ask: %.4f\n", *h_bestAsk[cur]);
            std::printf("VWAP: %.4f\n", *h_vwap[cur]);

            // switch buffer
            cur = (cur + 1) % nBuffers;
            filled = 0;
        }
    }

    // final partial batch
    if (filled > 0) {
        const int N = filled;
        gpuCheck(cudaMemcpyAsync(d_bids[cur], h_bids[cur], N * sizeof(float), cudaMemcpyHostToDevice, streams[cur]));
        gpuCheck(cudaMemcpyAsync(d_asks[cur], h_asks[cur], N * sizeof(float), cudaMemcpyHostToDevice, streams[cur]));
        gpuCheck(cudaMemcpyAsync(d_vols[cur], h_vols[cur], N * sizeof(float), cudaMemcpyHostToDevice, streams[cur]));

        orderbook_kernel<<<1, 32, 0, streams[cur]>>>(d_bids[cur], d_asks[cur], d_vols[cur], N,
                                                    d_bestBid[cur], d_bestAsk[cur], d_vwap[cur]);
        gpuCheck(cudaGetLastError());

        gpuCheck(cudaMemcpyAsync(h_bestBid[cur], d_bestBid[cur], sizeof(float), cudaMemcpyDeviceToHost, streams[cur]));
        gpuCheck(cudaMemcpyAsync(h_bestAsk[cur], d_bestAsk[cur], sizeof(float), cudaMemcpyDeviceToHost, streams[cur]));
        gpuCheck(cudaMemcpyAsync(h_vwap[cur], d_vwap[cur], sizeof(float), cudaMemcpyDeviceToHost, streams[cur]));
        gpuCheck(cudaStreamSynchronize(streams[cur]));

        std::printf("Best Bid: %.4f\n", *h_bestBid[cur]);
        std::printf("Best Ask: %.4f\n", *h_bestAsk[cur]);
        std::printf("VWAP: %.4f\n", *h_vwap[cur]);
    }

    // cleanup
    for (int i = 0; i < nBuffers; ++i) {
        cudaFree(d_bids[i]);
        cudaFree(d_asks[i]);
        cudaFree(d_vols[i]);
        cudaFree(d_bestBid[i]);
        cudaFree(d_bestAsk[i]);
        cudaFree(d_vwap[i]);

        cudaFreeHost(h_bids[i]);
        cudaFreeHost(h_asks[i]);
        cudaFreeHost(h_vols[i]);

        cudaFreeHost(h_bestBid[i]);
        cudaFreeHost(h_bestAsk[i]);
        cudaFreeHost(h_vwap[i]);

        cudaStreamDestroy(streams[i]);
    }
}

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "--stream" || arg == "-") {
            // read normalized rows from stdin
            run_orderbook_streaming(std::cin);
            return 0;
        }
        // otherwise treat arg as path
        const char* path = argv[1];
        run_orderbook(path);
        return 0;
    }

    // default: read from sample CSV as before
    const char* path = "sample_data/orderbook.csv";
    run_orderbook(path);
    return 0;
}