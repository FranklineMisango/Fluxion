cmake_minimum_required(VERSION 3.18)
set(CMAKE_CUDA_ARCHITECTURES 75 80 86 89)

project(Fluxion LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CUDA_STANDARD 17)

add_executable(orderbook src/cuda_orderbook.cu src/gpu_utils.cu)
add_executable(montecarlo src/monte_carlo.cu src/gpu_utils.cu)
add_executable(covariance src/covariance.cu src/gpu_utils.cu)

target_include_directories(orderbook PRIVATE include)
target_include_directories(montecarlo PRIVATE include)
target_include_directories(covariance PRIVATE include)

target_link_libraries(montecarlo PRIVATE curand)
target_link_libraries(covariance PRIVATE cublas)