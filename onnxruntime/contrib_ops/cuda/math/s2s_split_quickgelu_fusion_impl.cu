// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cuda/cu_inc/common.cuh"
#include "contrib_ops/cuda/math/s2s_split_quickgelu_fusion_impl.h"

using namespace onnxruntime::cuda;

namespace onnxruntime {
namespace contrib {
namespace cuda {

namespace {

constexpr int kElementsPerThread = GridDim::maxElementsPerThread;
#ifdef USE_ROCM
constexpr int kThreadsPerBlock = 512;
#else
constexpr int kThreadsPerBlock = GridDim::maxThreadsPerBlock;
#endif

}  // namespace

template <typename T>
__device__ inline T QuickGeluCompute(const T inp1, const T inp2, const T alpha_val) {
  // if (std::is_same<T, float>::value) {
  //   printf("Input is float: %f\n", static_cast<float>(inp1));
  // } else if (std::is_same<T, double>::value) {
  //   printf("Input is double: %lf\n", static_cast<double>(inp1));
  // } else if (std::is_same<T, int>::value) {
  //   printf("Input is int: %d\n", static_cast<int>(inp1));
  // } else if (std::is_same<T, long>::value) {
  //   printf("Input is long: %ld\n", static_cast<long>(inp1));
  // } else if (std::is_same<T, long long>::value) {
  //   printf("Input is long long: %lld\n", static_cast<long long>(inp1));
  // } else if (std::is_same<T, half>::value) {
  //   printf("Input is half: %f\n", __half2float(inp1));
  // } else if (std::is_same<T, nv_bfloat16>::value) {
  //   printf("Input is bfloat16: %f\n", __bfloat162float(inp1));
  // } else {
  //   // Add more types if necessary
  //   printf("Unknown type\n");
  // }
  T v = inp2 * alpha_val;
  T one = static_cast<T>(1.f);
  T zero = static_cast<T>(0.f);
  T sigmoid = v >= zero ? one / (one + _Exp(-v)) : one - one / (one + _Exp(v));
  T quickgelu_out = inp2 * sigmoid;
  // printf("Inp1 val: %f\n", static_cast<float>(inp1));
  // printf("Inp2 val: %f\n", static_cast<float>(inp2));
  // printf("Alpha val: %f\n", static_cast<float>(alpha_val));
  // printf("v val: %f\n", static_cast<float>(v));
  // printf("Sigmoid val: %f\n", static_cast<float>(sigmoid));
  // printf("quickgelu_out: %f\n", static_cast<float>(quickgelu_out));  // Using %f for float, change format specifier if using double
  // printf("Final out: %f\n", static_cast<float>(inp1 * quickgelu_out));
  return inp1 * quickgelu_out;
}

template <typename T>
__global__ void S2SModelSplitQuickGeluKernel(const int dim, float alpha, const T* input, T* output) {
  // TODO: Should I use long int?
  int input_line_stride = dim * 2;
  int output_line_stride = dim;
  // int offset_in1 = blockIdx.x * input_line_stride + threadIdx.x*kElementsPerThread;
  // int offset_in2 = offset_in1 + dim;
  // int offset_out = blockIdx.x * output_line_stride + threadIdx.x*kElementsPerThread;
  CUDA_LONG offset_in1 = kElementsPerThread * kThreadsPerBlock * blockIdx.x + threadIdx.x;
  // CUDA_LONG offset_in2 = offset_in1 + dim;
  // CUDA_LONG offset_out = (offset_in1 + 1) / 2;
  T alpha_val = static_cast<T>(alpha);
  // New implementation
  // CUDA_LONG id = offset_in1;
  #pragma unroll
  for (int i = 0; i < kElementsPerThread; i++) {
    if (offset_in1 % (2 * dim) < dim) {
      output[(offset_in1 + 1) / 2] = QuickGeluCompute(input[offset_in1], input[offset_in1 + dim], alpha_val);
      offset_in1 += NumThreadsPerBlock;
    }
  }

  // Separate QuickGelu code in another fn
  // printf("Curr kElementsPerThread %d\n", kElementsPerThread);
  // printf("Curr blockIdx.x %d\n", blockIdx.x);
  // printf("Curr threadIdx.x %d\n", threadIdx.x);
  // printf("Curr offset_in1 %d\n", offset_in1);
  // printf("Curr offset_in2 %d\n", offset_in2);
  // printf("Curr offset_out %d\n", offset_out);
  // printf("Curr dim %d\n", dim);
  // std::cout << "Curr kElementsPerThread:" << kElementsPerThread << std::endl;
  // input_size - dim
  // 5x4 (input_size = 20), dim = 2
  // 20 - 2
  // output is 5x 10 (dim = 10)
  // 5 is row number
  // 1 cuda block will process 1 row
  // kElementsPerThread
  // dim = 10K
  //
  // if threadIdx.x*kElementsPerThread < dim
  // int max_inp = 20 - dim;
  // What about this condition? (Removing if condition should improve Warp Divergence?)
  // for (int i = 0; i < kElementsPerThread; i++) {
  /*
  for (int i = 0; i < kElementsPerThread && threadIdx.x*kElementsPerThread + i < dim; i++){
    // int curr_in = offset_in1 + i;
    // int curr_half = curr_in / dim;
    // printf("Curr Inp Outside %d\n", curr_in);
    if (threadIdx.x*kElementsPerThread + i < dim) {
      // printf("Curr Inp inside %d\n", curr_in);
      output[offset_out + i] = QuickGeluCompute(input[offset_in1 + i], input[offset_in2+i], alpha_val);
      // T v = input[offset_in2+i] * alpha_val;
      // T sigmoid = v >= zero ? one / (one + _Exp(-v)) : one - one / (one + _Exp(v));
      // T quickgelu_out = input[offset_in2+i] * sigmoid;
      // output[offset_out + i] = input[offset_in1 + i] * quickgelu_out;
      // printf("Current output idx %d\n", offset_out + i);
      // printf("Current output value %f\n", quickgelu_out);
    }
  } */
}

template <typename T>
void LaunchS2SModelSplitQuickGeluKernel(cudaStream_t stream, int dim, int64_t input_size, float alpha, const T* input_data, T* output_data) {
  // CUDA_LONG N = static_cast<CUDA_LONG>(input_size);
  int num_threads_per_block = std::min<int>(static_cast<int>(CeilDiv(dim, kElementsPerThread)), kThreadsPerBlock);
  // TODO: num_blocks
  // 2x3x8
  // 6x8
  // dim =4
  // num_blocks = 6
  // 1Mx1Mx64
  // 10^12x64
  // num_blocks=10^12
  // max num_blocks = 65K
  // [1000][10]
  // ElemWiseKernel
  // [1000][1]
  int blocksPerGrid = static_cast<int>(CeilDiv(input_size, GridDim::maxThreadsPerBlock * GridDim::maxElementsPerThread));
  // int num_blocks = static_cast<int>(N/(2*dim));
  // printf("Num blocks %d\n", num_blocks);
  // printf("Final number threads per block %d\n", num_threads_per_block);
  // printf("Final num blocks %d\n", num_blocks);
  printf("Final blocksPerGrid %d\n", blocksPerGrid);
  printf("Final number threads per block %d\n", GridDim::maxThreadsPerBlock);
  // S2SModelSplitQuickGeluKernel<T><<<num_blocks, num_threads_per_block, 0, stream>>>(dim, alpha, input_data, output_data);
  S2SModelSplitQuickGeluKernel<T><<<blocksPerGrid, GridDim::maxThreadsPerBlock, 0, stream>>>(dim, alpha, input_data, output_data);
  // S2SModelSplitQuickGeluKernel<T><<<5, 1, 0, stream>>>(dim, input_data, output_data);
  // 4x10
  // output_dim = 5
  // 1st number of blocks = 4 (number of rows = 4)
  // 2nd size of block (threadsize) = ceil(5 / kElementsPerThread)
  // 3rd is don't need dynamic size
}

// explicit instantiations
#define SPECIALIZED_SplitQuickGelu_IMPL(T)                                                   \
  template void LaunchS2SModelSplitQuickGeluKernel<T>(cudaStream_t stream, int dim, int64_t input_size, \
                                                      float alpha, const T* input_data, T* output_data)

SPECIALIZED_SplitQuickGelu_IMPL(float);
SPECIALIZED_SplitQuickGelu_IMPL(half);
SPECIALIZED_SplitQuickGelu_IMPL(BFloat16);

#undef SPECIALIZED_SplitQuickGelu_IMPL

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
