#include <cstdio>
#include <cuda.h>

__inline__ __device__ float warpReduceMax(float val) {
    for (int offset = 16; offset > 0; offset /= 2)
        val = fmaxf(val, __shfl_down_sync(0xffffffff, val, offset));
    return val;
}

__inline__ __device__ float warpReduceMin(float val) {
    for (int offset = 16; offset > 0; offset /= 2)
        val = fminf(val, __shfl_down_sync(0xffffffff, val, offset));
    return val;
}

__global__ void orderbook_kernel(const float* bids, const float* asks,
                                 const float* volumes, int N,
                                 float* bestBid, float* bestAsk, float* vwap) 
{
    float localBid = -1e9;
    float localAsk =  1e9;
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

int main() {
    const int N = 1024;
    float *bids, *asks, *volumes;
    float *d_bids, *d_asks, *d_volumes;
    float bestBid, bestAsk, vwap;

    bids = new float[N];
    asks = new float[N];
    volumes = new float[N];

    for (int i = 0; i < N; i++) {
        bids[i] = 100.0f + (i % 10);
        asks[i] = 101.0f + (i % 10);
        volumes[i] = 1.0f;
    }

    cudaMalloc(&d_bids, N * sizeof(float));
    cudaMalloc(&d_asks, N * sizeof(float));
    cudaMalloc(&d_volumes, N * sizeof(float));

    cudaMemcpy(d_bids, bids, N * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_asks, asks, N * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_volumes, volumes, N * sizeof(float), cudaMemcpyHostToDevice);

    orderbook_kernel<<<1, 32>>>(d_bids, d_asks, d_volumes, N,
                                &bestBid, &bestAsk, &vwap);

    cudaDeviceSynchronize();

    printf("Best Bid: %.4f\n", bestBid);
    printf("Best Ask: %.4f\n", bestAsk);
    printf("VWAP: %.4f\n", vwap);

    cudaFree(d_bids);
    cudaFree(d_asks);
    cudaFree(d_volumes);
    delete[] bids;
    delete[] asks;
    delete[] volumes;

    return 0;
}
