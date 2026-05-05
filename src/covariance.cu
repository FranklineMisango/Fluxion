#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <cublas_v2.h>

#include "covariance.hpp"
#include "gpu_utils.hpp"

void run_covariance(const char* path) {
    std::vector<float> data;
    int rows = 0;
    int cols = 0;

    std::ifstream file(path);
    std::string line;

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        float val = 0.0f;
        int colCount = 0;

        while (ss >> val) {
            data.push_back(val);
            colCount++;
        }

        if (colCount > 0) {
            cols = colCount;
            rows++;
        }
    }

    if (rows == 0 || cols == 0) {
        std::fprintf(stderr, "No covariance data loaded from %s\n", path);
        return;
    }

    float* d_X = nullptr;
    float* d_cov = nullptr;
    gpuCheck(cudaMalloc(&d_X, rows * cols * sizeof(float)));
    gpuCheck(cudaMalloc(&d_cov, cols * cols * sizeof(float)));

    gpuCheck(cudaMemcpy(d_X, data.data(), rows * cols * sizeof(float), cudaMemcpyHostToDevice));

    cublasHandle_t handle;
    cublasCreate(&handle);

    const float alpha = 1.0f / (rows - 1);
    const float beta = 0.0f;

    cublasSgemm(handle,
                CUBLAS_OP_T, CUBLAS_OP_N,
                cols, cols, rows,
                &alpha,
                d_X, rows,
                d_X, rows,
                &beta,
                d_cov, cols);

    gpuCheck(cudaDeviceSynchronize());
    std::printf("Covariance matrix computed (%dx%d).\n", cols, cols);

    cublasDestroy(handle);
    cudaFree(d_X);
    cudaFree(d_cov);
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "sample_data/returns.csv";
    run_covariance(path);
    return 0;
}