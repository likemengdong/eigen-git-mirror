// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2014 Benoit Steiner <benoit.steiner.goog@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_CXX11_TENSOR_TENSOR_EXECUTOR_H
#define EIGEN_CXX11_TENSOR_TENSOR_EXECUTOR_H

#ifdef EIGEN_USE_THREADS
#include <future>
#endif

namespace Eigen {

/** \class TensorExecutor
  * \ingroup CXX11_Tensor_Module
  *
  * \brief The tensor executor class.
  *
  * This class is responsible for launch the evaluation of the expression on
  * the specified computing device.
  */
namespace internal {

// Default strategy: the expression is evaluated with a single cpu thread.
template<typename Expression, typename Device = DefaultDevice, bool Vectorizable = TensorEvaluator<Expression, Device>::PacketAccess>
class TensorExecutor
{
 public:
  typedef typename Expression::Index Index;
  EIGEN_DEVICE_FUNC
  static inline void run(const Expression& expr, const Device& device = Device())
  {
    TensorEvaluator<Expression, Device> evaluator(expr, device);
    const bool needs_assign = evaluator.evalSubExprsIfNeeded(NULL);
    if (needs_assign)
    {
      const Index size = evaluator.dimensions().TotalSize();
      for (Index i = 0; i < size; ++i) {
        evaluator.evalScalar(i);
      }
    }
    evaluator.cleanup();
  }
};


template<typename Expression>
class TensorExecutor<Expression, DefaultDevice, true>
{
 public:
  typedef typename Expression::Index Index;
  static inline void run(const Expression& expr, const DefaultDevice& device = DefaultDevice())
  {
    TensorEvaluator<Expression, DefaultDevice> evaluator(expr, device);
    const bool needs_assign = evaluator.evalSubExprsIfNeeded(NULL);
    if (needs_assign)
    {
      const Index size = evaluator.dimensions().TotalSize();
      static const int PacketSize = unpacket_traits<typename TensorEvaluator<Expression, DefaultDevice>::PacketReturnType>::size;
      const int VectorizedSize = (size / PacketSize) * PacketSize;

      for (Index i = 0; i < VectorizedSize; i += PacketSize) {
        evaluator.evalPacket(i);
      }
      for (Index i = VectorizedSize; i < size; ++i) {
        evaluator.evalScalar(i);
      }
    }
    evaluator.cleanup();
  }
};



// Multicore strategy: the index space is partitioned and each partition is executed on a single core
#ifdef EIGEN_USE_THREADS
template <typename Evaluator, typename Index, bool Vectorizable = Evaluator::PacketAccess>
struct EvalRange {
  static void run(Evaluator* evaluator, const Index first, const Index last) {
    eigen_assert(last > first);
    for (Index i = first; i < last; ++i) {
      evaluator->evalScalar(i);
    }
  }
};

template <typename Evaluator, typename Index>
struct EvalRange<Evaluator, Index, true> {
  static void run(Evaluator* evaluator, const Index first, const Index last) {
    eigen_assert(last > first);

    Index i = first;
    static const int PacketSize = unpacket_traits<typename Evaluator::PacketReturnType>::size;
    if (last - first > PacketSize) {
      eigen_assert(first % PacketSize == 0);
      Index lastPacket = last - (last % PacketSize);
      for (; i < lastPacket; i += PacketSize) {
        evaluator->evalPacket(i);
      }
    }

    for (; i < last; ++i) {
      evaluator->evalScalar(i);
    }
  }
};

template<typename Expression, bool Vectorizable>
class TensorExecutor<Expression, ThreadPoolDevice, Vectorizable>
{
 public:
  typedef typename Expression::Index Index;
  static inline void run(const Expression& expr, const ThreadPoolDevice& device)
  {
    typedef TensorEvaluator<Expression, ThreadPoolDevice> Evaluator;
    Evaluator evaluator(expr, device);
    const bool needs_assign = evaluator.evalSubExprsIfNeeded(NULL);
    if (needs_assign)
    {
      const Index size = evaluator.dimensions().TotalSize();

      static const int PacketSize = Vectorizable ? unpacket_traits<typename Evaluator::PacketReturnType>::size : 1;

      int blocksz = std::ceil<int>(static_cast<float>(size)/device.numThreads()) + PacketSize - 1;
      const Index blocksize = std::max<Index>(PacketSize, (blocksz - (blocksz % PacketSize)));
      const Index numblocks = size / blocksize;

      Index i = 0;
      vector<std::future<void> > results;
      results.reserve(numblocks);
      for (int i = 0; i < numblocks; ++i) {
         results.push_back(std::async(std::launch::async, &EvalRange<Evaluator, Index>::run, &evaluator, i*blocksize, (i+1)*blocksize));
      }

      for (int i = 0; i < numblocks; ++i) {
        results[i].get();
      }

      if (numblocks * blocksize < size) {
        EvalRange<Evaluator, Index>::run(&evaluator, numblocks * blocksize, size);
      }
    }
    evaluator.cleanup();
  }
};
#endif


// GPU: the evaluation of the expression is offloaded to a GPU.
#if defined(EIGEN_USE_GPU) && defined(__CUDACC__)
template <typename Evaluator>
__global__ void EigenMetaKernel(Evaluator eval, unsigned int size) {
  const int first_index = blockIdx.x * blockDim.x + threadIdx.x;
  const int step_size = blockDim.x * gridDim.x;
  for (int i = first_index; i < size; i += step_size) {
    eval.evalScalar(i);
  }
}

template<typename Expression, bool Vectorizable>
class TensorExecutor<Expression, GpuDevice, Vectorizable>
{
 public:
  typedef typename Expression::Index Index;
  static inline void run(const Expression& expr, const GpuDevice& device)
  {
    TensorEvaluator<Expression, GpuDevice> evaluator(expr, device);
    const bool needs_assign = evaluator.evalSubExprsIfNeeded(NULL);
    if (needs_assign)
    {
      const int num_blocks = getNumCudaMultiProcessors() * maxCudaThreadsPerMultiProcessor() / maxCudaThreadsPerBlock();
      const int block_size = maxCudaThreadsPerBlock();

      const Index size = evaluator.dimensions().TotalSize();
      EigenMetaKernel<TensorEvaluator<Expression, GpuDevice> > <<<num_blocks, block_size, 0, device.stream()>>>(evaluator, size);
      assert(cudaGetLastError() == cudaSuccess);
    }
    evaluator.cleanup();
  }
};
#endif

} // end namespace internal

} // end namespace Eigen

#endif // EIGEN_CXX11_TENSOR_TENSOR_EXECUTOR_H
