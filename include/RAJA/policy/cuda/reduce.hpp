/*!
 ******************************************************************************
 *
 * \file
 *
 * \brief   Header file containing RAJA reduction templates for CUDA execution.
 *
 *          These methods should work on any platform that supports
 *          CUDA devices.
 *
 ******************************************************************************
 */

#ifndef RAJA_reduce_cuda_HPP
#define RAJA_reduce_cuda_HPP

#include "RAJA/config.hpp"

#if defined(RAJA_ENABLE_CUDA)

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) 2016, Lawrence Livermore National Security, LLC.
//
// Produced at the Lawrence Livermore National Laboratory
//
// LLNL-CODE-689114
//
// All rights reserved.
//
// This file is part of RAJA.
//
// For additional details, please also read RAJA/LICENSE.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the disclaimer below.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the disclaimer (as noted below) in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of the LLNS/LLNL nor the names of its contributors may
//   be used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL LAWRENCE LIVERMORE NATIONAL SECURITY,
// LLC, THE U.S. DEPARTMENT OF ENERGY OR CONTRIBUTORS BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
// IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#include "RAJA/util/types.hpp"

#include "RAJA/util/basic_mempool.hpp"

#include "RAJA/pattern/reduce.hpp"

#include "RAJA/policy/cuda/MemUtils_CUDA.hpp"

#include "RAJA/policy/cuda/policy.hpp"

#include "RAJA/policy/cuda/atomic.hpp"

#include "RAJA/policy/cuda/raja_cudaerrchk.hpp"

#include <cuda.h>

#include "RAJA/util/mutex.hpp"

namespace RAJA
{

namespace reduce
{

namespace cuda
{
//! atomic operator version of Reducer object
template <typename Reducer>
struct atomic;

template <typename T>
struct atomic<sum<T>>
{
  RAJA_DEVICE RAJA_INLINE
  void operator()(T &val, const T v)
  {
    RAJA::cuda::atomicAdd(&val, v);
  }
};

template <typename T>
struct atomic<min<T>>
{
  RAJA_DEVICE RAJA_INLINE
  void operator()(T &val, const T v)
  {
    RAJA::cuda::atomicMin(&val, v);
  }
};

template <typename T>
struct atomic<max<T>>
{
  RAJA_DEVICE RAJA_INLINE
  void operator()(T &val, const T v)
  {
    RAJA::cuda::atomicMax(&val, v);
  }
};

} // namespace cuda

} // namespace reduce

namespace cuda
{

namespace impl
{
/*!
 ******************************************************************************
 *
 * \brief Method to shuffle 32b registers in sum reduction for arbitrary type.
 *
 * \Note Returns an undefined value if src lane is inactive (divergence).
 *       Returns this lane's value if src lane is out of bounds or has exited.
 *
 ******************************************************************************
 */
template <typename T>
RAJA_DEVICE RAJA_INLINE
T shfl_xor_sync(T var, int laneMask)
{
  const int int_sizeof_T = (sizeof(T) + sizeof(int) - 1) / sizeof(int);
  union {
    T var;
    int arr[int_sizeof_T];
  } Tunion;
  Tunion.var = var;

  for (int i = 0; i < int_sizeof_T; ++i) {
#if (__CUDACC_VER_MAJOR__ >= 9)
    Tunion.arr[i] = ::__shfl_xor_sync(Tunion.arr[i], laneMask);
#else
    Tunion.arr[i] = ::__shfl_xor(Tunion.arr[i], laneMask);
#endif
  }
  return Tunion.var;
}

template <typename T>
RAJA_DEVICE RAJA_INLINE
T shfl_sync(T var, int srcLane)
{
  const int int_sizeof_T = (sizeof(T) + sizeof(int) - 1) / sizeof(int);
  union {
    T var;
    int arr[int_sizeof_T];
  } Tunion;
  Tunion.var = var;

  for (int i = 0; i < int_sizeof_T; ++i) {
#if (__CUDACC_VER_MAJOR__ >= 9)
    Tunion.arr[i] = ::__shfl_sync(Tunion.arr[i], srcLane);
#else
    Tunion.arr[i] = ::__shfl(Tunion.arr[i], srcLane);
#endif
  }
  return Tunion.var;
}

template <typename T>
RAJA_HOST_DEVICE RAJA_INLINE
bool bitwize_zero(T var)
{
  const int int_sizeof_T = (sizeof(T) + sizeof(int) - 1) / sizeof(int);
  union myUnion {
    T var;
    int arr[int_sizeof_T];
    RAJA_HOST_DEVICE
    constexpr myUnion() : arr{0} {}
  } Tunion{};
  Tunion.var = var;

  int zero = 0;
  for (int i = 0; i < int_sizeof_T; ++i) {
    zero |= Tunion.arr[i];
  }
  return zero == 0;
};

//! reduce values in block into thread 0
template <typename Reducer, typename T>
RAJA_DEVICE RAJA_INLINE
T block_reduce(T val)
{
  int numThreads = blockDim.x * blockDim.y * blockDim.z;

  int threadId = threadIdx.x + blockDim.x * threadIdx.y
                 + (blockDim.x * blockDim.y) * threadIdx.z;

  int warpId = threadId % WARP_SIZE;
  int warpNum = threadId / WARP_SIZE;

  T temp = val;

  if (numThreads % WARP_SIZE == 0) {

    // reduce each warp
    for (int i = 1; i < WARP_SIZE ; i *= 2) {
      T rhs = shfl_xor_sync(temp, i);
      Reducer{}(temp, rhs);
    }

  } else {

    // reduce each warp
    for (int i = 1; i < WARP_SIZE ; i *= 2) {
      int srcLane = threadId ^ i;
      T rhs = shfl_sync(temp, srcLane);
      // only add from threads that exist (don't double count own value)
      if (srcLane < numThreads) {
        Reducer{}(temp, rhs);
      }
    }

  }

  // reduce per warp values
  if (numThreads > WARP_SIZE) {

    __shared__ T sd[MAX_WARPS];

    // write per warp values to shared memory
    if (warpId == 0) {
      sd[warpNum] = temp;
    }

    __syncthreads();

    if (warpNum == 0) {

      // read per warp values
      if (warpId*WARP_SIZE < numThreads) {
        temp = sd[warpId];
      } else {
        temp = Reducer::identity;
      }

      for (int i = 1; i < WARP_SIZE ; i *= 2) {
        T rhs = shfl_xor_sync(temp, i);
        Reducer{}(temp, rhs);
      }
    }

    __syncthreads();

  }

  return temp;
}

//! reduce values in block into thread 0
template <typename Reducer, typename T, typename IndexType>
RAJA_DEVICE RAJA_INLINE
cuda::LocType<T, IndexType> block_reduce(cuda::LocType<T, IndexType> val)
{
  int numThreads = blockDim.x * blockDim.y * blockDim.z;

  int threadId = threadIdx.x + blockDim.x * threadIdx.y
                 + (blockDim.x * blockDim.y) * threadIdx.z;

  int warpId = threadId % WARP_SIZE;
  int warpNum = threadId / WARP_SIZE;

  cuda::LocType<T, IndexType> temp = val;

  if (numThreads % WARP_SIZE == 0) {

    // reduce each warp
    for (int i = 1; i < WARP_SIZE ; i *= 2) {
      T rhs_val = shfl_xor_sync(temp.val, i);
      IndexType rhs_idx = shfl_xor_sync(temp.idx, i);
      Reducer{}(temp.val, temp.idx, rhs_val, rhs_idx);
    }

  } else {

    // reduce each warp
    for (int i = 1; i < WARP_SIZE ; i *= 2) {
      int srcLane = threadId ^ i;
      T rhs_val = shfl_sync(temp.val, srcLane);
      IndexType rhs_idx = shfl_sync(temp.idx, srcLane);
      // only add from threads that exist (don't double count own value)
      if (srcLane < numThreads) {
        Reducer{}(temp.val, temp.idx, rhs_val, rhs_idx);
      }
    }

  }

  // reduce per warp values
  if (numThreads > WARP_SIZE) {

    __shared__ T sd_val[MAX_WARPS];
    __shared__ IndexType sd_idx[MAX_WARPS];

    // write per warp values to shared memory
    if (warpId == 0) {
      sd_val[warpNum] = temp.val;
      sd_idx[warpNum] = temp.idx;
    }

    __syncthreads();

    if (warpNum == 0) {

      // read per warp values
      if (warpId*WARP_SIZE < numThreads) {
        temp.val = sd_val[warpId];
        temp.idx = sd_idx[warpId];
      } else {
        temp.val = Reducer::identity;
        temp.idx = IndexType{-1};
      }

      for (int i = 1; i < WARP_SIZE ; i *= 2) {
        T rhs_val = shfl_xor_sync(temp.val, i);
        IndexType rhs_idx = shfl_xor_sync(temp.idx, i);
        Reducer{}(temp.val, temp.idx, rhs_val, rhs_idx);
      }
    }

    __syncthreads();

  }

  return temp;
}

template <typename Reducer, typename T>
RAJA_DEVICE RAJA_INLINE
bool setup_grid_reduce(T* RAJA_UNUSED_ARG(device_mem),
                       unsigned int* RAJA_UNUSED_ARG(device_count))
{
  return true;
}

//! reduce values in grid into thread 0 of last running block
//  returns true if put reduced value in val
template <typename Reducer, typename T>
RAJA_DEVICE RAJA_INLINE
bool grid_reduce(T& val,
                 T* device_mem,
                 unsigned int* device_count)
{
  int numBlocks = gridDim.x * gridDim.y * gridDim.z;
  int numThreads = blockDim.x * blockDim.y * blockDim.z;
  unsigned int wrap_around = numBlocks - 1;

  int blockId = blockIdx.x + gridDim.x * blockIdx.y
                + (gridDim.x * gridDim.y) * blockIdx.z;

  int threadId = threadIdx.x + blockDim.x * threadIdx.y
                 + (blockDim.x * blockDim.y) * threadIdx.z;

  T temp = block_reduce<Reducer>(val);

  bool lastBlock = false;

  if (numBlocks == 1) {

    lastBlock = true;

    if (threadId == 0) {
      val = temp;
    }

  } else {

    // one thread per block writes to device_mem
    if (threadId == 0) {
      device_mem[blockId] = temp;
      // ensure write visible to all threadblocks
      __threadfence();
      // increment counter, (wraps back to zero if old count == wrap_around)
      unsigned int old_count = ::atomicInc(device_count, wrap_around);
      lastBlock = (old_count == wrap_around);
    }

    // returns non-zero value if any thread passes in a non-zero value
    lastBlock = __syncthreads_or(lastBlock);

    // last block accumulates values from device_mem
    if (lastBlock) {
      temp = Reducer::identity;

      for (int i = threadId; i < numBlocks; i += numThreads) {
        Reducer{}(temp, device_mem[i]);
      }

      temp = block_reduce<Reducer>(temp);

      // one thread returns value
      if (threadId == 0) {
        val = temp;
      }
    }
  }

  return lastBlock && threadId == 0;
}


template <typename Reducer, typename T>
RAJA_DEVICE RAJA_INLINE
bool setup_grid_reduce_atomic(T* device_mem,
                              unsigned int* device_count)
{
  int numBlocks = gridDim.x * gridDim.y * gridDim.z;

  int threadId = threadIdx.x + blockDim.x * threadIdx.y
                 + (blockDim.x * blockDim.y) * threadIdx.z;

  if ( !bitwize_zero(Reducer::identity) && numBlocks != 1) {

    // one thread in first block initializes device_mem
    if (threadId == 0) {
      unsigned int old_val = ::atomicCAS(device_count, 0u, 1u);
      if (old_val == 0u) {
        *device_mem = Reducer::identity;
        __threadfence();
        ::atomicAdd(device_count, 1u);
      }
    }
  }

  return true;
}

//! reduce values in grid into thread 0 of last running block
//  returns true if put reduced value in val
template <typename Reducer, typename T>
RAJA_DEVICE RAJA_INLINE
bool grid_reduce_atomic(T& val,
                        T* device_mem,
                        unsigned int* device_count)
{
  int numBlocks = gridDim.x * gridDim.y * gridDim.z;
  unsigned int wrap_around = numBlocks + 1;

  int threadId = threadIdx.x + blockDim.x * threadIdx.y
                 + (blockDim.x * blockDim.y) * threadIdx.z;

  bool lastBlock = false;
  if (numBlocks == 1) {

    T temp = block_reduce<Reducer>(val);

    if (threadId == 0) {
      lastBlock = true;
      val = temp;
    }

  } else {

    T temp = block_reduce<Reducer>(val);

    // one thread per block performs atomic on device_mem
    if (threadId == 0) {

      if ( !bitwize_zero(Reducer::identity) ) {
        // thread waits for device_mem to be initialized
        while(static_cast<volatile unsigned int*>(device_count)[0] < 2u);
        __threadfence();
      }
      RAJA::reduce::cuda::atomic<Reducer>{}(*device_mem, temp);
      __threadfence();
      // increment counter, (wraps back to zero if old count == wrap_around)
      unsigned int old_count = ::atomicInc(device_count, wrap_around);
      lastBlock = (old_count == wrap_around);

      // last block gets value from device_mem
      if (lastBlock) {
        val = *device_mem;
      }
    }

  }

  return lastBlock;
}

template <typename Reducer, typename T, typename IndexType>
RAJA_DEVICE RAJA_INLINE
bool setup_grid_reduceLoc(T* RAJA_UNUSED_ARG(device_mem),
                          IndexType* RAJA_UNUSED_ARG(deviceLoc_mem),
                          unsigned int* RAJA_UNUSED_ARG(device_count))
{
  return true;
}

//! reduce values in grid into thread 0 of last running block
//  returns true if put reduced value in val
template <typename Reducer, typename T, typename IndexType>
RAJA_DEVICE RAJA_INLINE
bool grid_reduceLoc(cuda::LocType<T, IndexType>& val,
                    T* device_mem,
                    IndexType* deviceLoc_mem,
                    unsigned int* device_count)
{
  int numBlocks = gridDim.x * gridDim.y * gridDim.z;
  int numThreads = blockDim.x * blockDim.y * blockDim.z;
  unsigned int wrap_around = numBlocks - 1;

  int blockId = blockIdx.x + gridDim.x * blockIdx.y
                + (gridDim.x * gridDim.y) * blockIdx.z;

  int threadId = threadIdx.x + blockDim.x * threadIdx.y
                 + (blockDim.x * blockDim.y) * threadIdx.z;

  cuda::LocType<T, IndexType> temp = block_reduce<Reducer>(val);

  bool lastBlock = false;

  if (numBlocks == 1) {

    lastBlock = true;

    if (threadId == 0) {
      val = temp;
    }

  } else {

    // one thread per block writes to device_mem
    if (threadId == 0) {
      device_mem[blockId]    = temp.val;
      deviceLoc_mem[blockId] = temp.idx;
      // ensure write visible to all threadblocks
      __threadfence();
      // increment counter, (wraps back to zero if old count == wrap_around)
      unsigned int old_count = ::atomicInc(device_count, wrap_around);
      lastBlock = (old_count == wrap_around);
    }

    // returns non-zero value if any thread passes in a non-zero value
    lastBlock = __syncthreads_or(lastBlock);

    // last block accumulates values from device_mem
    if (lastBlock) {
      temp.val = Reducer::identity;
      temp.idx = IndexType(-1);

      for (int i = threadId; i < numBlocks; i += numThreads) {
        Reducer{}(temp.val,      temp.idx,
                  device_mem[i], deviceLoc_mem[i]);
      }

      temp = block_reduce<Reducer>(temp);

      // one thread returns value
      if (threadId == 0) {
        val = temp;
      }
    }

  }

  return lastBlock && threadId == 0;
}

}  // namespace impl

//! Object that manages pinned memory buffers for reduction results
//  use one per reducer object
template <typename T>
class PinnedTally
{
public:
  //! Object put in Pinned memory with value and pointer to next Node
  struct Node {
    Node* next;
    T value;
  };
  //! Object per stream to keep track of pinned memory nodes
  struct StreamNode {
    StreamNode* next;
    cudaStream_t stream;
    Node* node_list;
  };

  //! Iterator over streams used by reducer
  class StreamIterator {
  public:
    StreamIterator() = delete;

    StreamIterator(StreamNode* sn)
      : m_sn(sn)
    {
    }

    const StreamIterator& operator++()
    {
      m_sn = m_sn->next;
      return *this;
    }

    StreamIterator operator++(int)
    {
      StreamIterator ret = *this;
      this->operator++();
      return ret;
    }

    cudaStream_t& operator*()
    {
      return m_sn->stream;
    }

    bool operator==(const StreamIterator& rhs) const
    {
      return m_sn == rhs.m_sn;
    }

    bool operator!=(const StreamIterator& rhs) const
    {
      return !this->operator==(rhs);
    }

  private:
    StreamNode* m_sn;
  };

  //! Iterator over all values generated by reducer
  class StreamNodeIterator {
  public:
    StreamNodeIterator() = delete;

    StreamNodeIterator(StreamNode* sn, Node* n)
      : m_sn(sn), m_n(n)
    {
    }

    const StreamNodeIterator& operator++()
    {
      if (m_n->next) {
        m_n = m_n->next;
      } else if (m_sn->next) {
        m_sn = m_sn->next;
        m_n = m_sn->node_list;
      } else {
        m_sn = nullptr;
        m_n = nullptr;
      }
      return *this;
    }

    StreamNodeIterator operator++(int)
    {
      StreamNodeIterator ret = *this;
      this->operator++();
      return ret;
    }

    T& operator*()
    {
      return m_n->value;
    }

    bool operator==(const StreamNodeIterator& rhs) const
    {
      return m_n == rhs.m_n;
    }

    bool operator!=(const StreamNodeIterator& rhs) const
    {
      return !this->operator==(rhs);
    }

  private:
    StreamNode* m_sn;
    Node* m_n;
  };

  PinnedTally()
    : stream_list(nullptr)
  {

  }

  PinnedTally(const PinnedTally&) = delete;

  //! get begin iterator over streams
  StreamIterator streamBegin()
  {
    return{stream_list};
  }

  //! get end iterator over streams
  StreamIterator streamEnd()
  {
    return{nullptr};
  }

  //! get begin iterator over values
  StreamNodeIterator begin()
  {
    return{stream_list, stream_list ? stream_list->node_list : nullptr};
  }

  //! get end iterator over values
  StreamNodeIterator end()
  {
    return{nullptr, nullptr};
  }

  //! get new value for use in stream
  T* new_value(cudaStream_t stream)
  {
#if defined(RAJA_ENABLE_OPENMP) && defined(_OPENMP)
    lock_guard<omp::mutex> lock(m_mutex);
#endif
    StreamNode* sn = stream_list;
    while(sn) {
      if (sn->stream == stream) break;
      sn = sn->next;
    }
    if (!sn) {
      sn = (StreamNode*)malloc(sizeof(StreamNode));
      sn->next = stream_list;
      sn->stream = stream;
      sn->node_list = nullptr;
      stream_list = sn;
    }
    Node* n = cuda::pinned_mempool_type::getInstance().malloc<Node>(1);
    n->next = sn->node_list;
    sn->node_list = n;
    return &n->value;
  }

  //! all values used in all streams
  void free_list()
  {
    while (stream_list) {
      StreamNode* s = stream_list;
      while (s->node_list) {
        Node* n = s->node_list;
        s->node_list = n->next;
        cuda::pinned_mempool_type::getInstance().free(n);
      }
      stream_list = s->next;
      free(s);
    }
  }

  ~PinnedTally()
  {
    free_list();
  }

#if defined(RAJA_ENABLE_OPENMP) && defined(_OPENMP)
  omp::mutex m_mutex;
#endif

private:
  StreamNode* stream_list;
};

//
//////////////////////////////////////////////////////////////////////
//
// Reduction classes.
//
//////////////////////////////////////////////////////////////////////
//

//! Information necessary for Cuda offload to be considered
struct Offload_Info {

  // Offload_Info() = delete;
  Offload_Info() = default;

  RAJA_HOST_DEVICE
  Offload_Info(const Offload_Info &)
  {
  }
};

//! Reduction data for Cuda Offload -- stores value, host pointer, and device pointer
template <bool Async, typename Reducer, typename T>
struct Reduce_Data {
  //! union to hold either pointer to PinnedTally or poiter to value
  //  only use list before setup for device and only use val_ptr after
  union tally_u {
    PinnedTally<T>* list;
    T *val_ptr;
    tally_u(PinnedTally<T>* l) : list(l) {};
    tally_u(T *v_ptr) : val_ptr(v_ptr) {};
  };

  mutable T value;
  tally_u tally_or_val_ptr;
  unsigned int *device_count;
  T *device;
  bool own_device_ptr;

  //! disallow default constructor
  Reduce_Data() = delete;

  /*! \brief create from a default value and offload information
   *
   *  allocates PinnedTally to hold device values
   */
  explicit Reduce_Data(T initValue)
      : value{initValue},
        tally_or_val_ptr{new PinnedTally<T>},
        device_count{nullptr},
        device{nullptr},
        own_device_ptr{false}
  {
  }

  RAJA_HOST_DEVICE
  Reduce_Data(const Reduce_Data &other)
      : value{Reducer::identity},
        tally_or_val_ptr{other.tally_or_val_ptr},
        device_count{other.device_count},
        device{other.device},
        own_device_ptr{false}
  {
  }

  //! delete pinned tally
  RAJA_HOST_DEVICE
  RAJA_INLINE
  void destroy()
  {
#if !defined(__CUDA_ARCH__)
    delete tally_or_val_ptr.list; tally_or_val_ptr.list = nullptr;
#else
    T temp = value;
    if (impl::grid_reduce<Reducer>(temp, device,
                                   device_count)) {
      printf("writing %e\n", temp);
      tally_or_val_ptr.val_ptr[0] = temp;
    }
#endif
  }

  //! check and setup for device
  //  allocate device pointers and get a new result buffer from the pinned tally
  RAJA_HOST_DEVICE
  RAJA_INLINE
  bool setupForDevice(Offload_Info &info)
  {
    bool act = false;
#if !defined(__CUDA_ARCH__)
    act = !device && setupReducers();
    if (act) {
      dim3 gridDim = currentGridDim();
      size_t numBlocks = gridDim.x * gridDim.y * gridDim.z;
      device = device_mempool_type::getInstance().malloc<T>(numBlocks);
      device_count = device_zeroed_mempool_type::getInstance().malloc<unsigned int>(1);
      tally_or_val_ptr.val_ptr = tally_or_val_ptr.list->new_value(currentStream());
      own_device_ptr = true;
    }
#else
    act = impl::setup_grid_reduce<Reducer>(device,
                                           device_count);
#endif
    return act;
  }

  //! if own resources teardown device setup
  //  free device pointers
  RAJA_INLINE
  void teardownForDevice(Offload_Info&)
  {
    if(own_device_ptr) {
      device_mempool_type::getInstance().free(device);  device = nullptr;
      device_zeroed_mempool_type::getInstance().free(device_count);  device_count = nullptr;
      tally_or_val_ptr.val_ptr = nullptr;
      own_device_ptr = false;
    }
  }

  //! transfers from the host to the device
  RAJA_INLINE
  void hostToDevice(Offload_Info &)
  {
  }

  //! transfers from the device to the host
  RAJA_INLINE
  void deviceToHost(Offload_Info &)
  {
    auto end = tally_or_val_ptr.list->streamEnd();
    for(auto s = tally_or_val_ptr.list->streamBegin(); s != end; ++s) {
      synchronize(*s);
    }
  }

  //! frees all data used
  //  frees all values in the pinned tally
  RAJA_INLINE
  void cleanup(Offload_Info &)
  {
    tally_or_val_ptr.list->free_list();
  }
};


//! Reduction data for Cuda Offload -- stores value, host pointer
template <bool Async, typename Reducer, typename T>
struct ReduceAtomic_Data {
  //! union to hold either pointer to PinnedTally or poiter to value
  //  only use list before setup for device and only use val_ptr after
  union tally_u {
    PinnedTally<T>* list;
    T *val_ptr;
    tally_u(PinnedTally<T>* l) : list(l) {};
    tally_u(T *v_ptr) : val_ptr(v_ptr) {};
  };

  mutable T value;
  tally_u tally_or_val_ptr;
  unsigned int* device_count;
  T* device;
  bool own_device_ptr;

  //! disallow default constructor
  ReduceAtomic_Data() = delete;

  /*! \brief create from a default value and offload information
   *
   *  allocates PinnedTally to hold device values
   */
  explicit ReduceAtomic_Data(T initValue)
      : value{initValue},
        tally_or_val_ptr{new PinnedTally<T>},
        device_count{nullptr},
        device{nullptr},
        own_device_ptr{false}
  {
  }

  RAJA_HOST_DEVICE
  ReduceAtomic_Data(const ReduceAtomic_Data &other)
      : value{Reducer::identity},
        tally_or_val_ptr{other.tally_or_val_ptr},
        device_count{other.device_count},
        device{other.device},
        own_device_ptr{false}
  {
  }

  //! delete pinned tally
  RAJA_HOST_DEVICE
  RAJA_INLINE
  void destroy()
  {
#if !defined(__CUDA_ARCH__)
    delete tally_or_val_ptr.list; tally_or_val_ptr.list = nullptr;
#else
    T temp = value;
    if (impl::grid_reduce_atomic<Reducer>(temp, device,
                                          device_count)) {
      printf("writing %e\n", temp);
      tally_or_val_ptr.val_ptr[0] = temp;
    }
#endif
  }

  //! check and setup for device
  //  allocate device pointers and get a new result buffer from the pinned tally
  RAJA_HOST_DEVICE
  RAJA_INLINE
  bool setupForDevice(Offload_Info &info)
  {
    bool act = false;
#if !defined(__CUDA_ARCH__)
    act = !device && setupReducers();
    if (act) {
      device = device_zeroed_mempool_type::getInstance().malloc<T>(1);
      device_count = device_zeroed_mempool_type::getInstance().malloc<unsigned int>(1);
      tally_or_val_ptr.val_ptr = tally_or_val_ptr.list->new_value(currentStream());
      own_device_ptr = true;
    }
#else
    act = impl::setup_grid_reduce_atomic<Reducer>(device,
                                                  device_count);
#endif
    return act;
  }

  //! if own resources teardown device setup
  //  free device pointers
  RAJA_INLINE
  void teardownForDevice(Offload_Info&)
  {
    if(own_device_ptr) {
      device_zeroed_mempool_type::getInstance().free(device);  device = nullptr;
      device_zeroed_mempool_type::getInstance().free(device_count);  device_count = nullptr;
      tally_or_val_ptr.val_ptr = nullptr;
      own_device_ptr = false;
    }
  }

  //! transfers from the host to the device
  RAJA_INLINE
  void hostToDevice(Offload_Info &)
  {
  }

  //! transfers from the device to the host
  RAJA_INLINE
  void deviceToHost(Offload_Info &)
  {
    auto end = tally_or_val_ptr.list->streamEnd();
    for(auto s = tally_or_val_ptr.list->streamBegin(); s != end; ++s) {
      synchronize(*s);
    }
  }

  //! frees all data used
  //  frees all values in the pinned tally
  RAJA_INLINE
  void cleanup(Offload_Info &)
  {
    tally_or_val_ptr.list->free_list();
  }
};

//! Reduction data for Cuda Offload -- stores value, host pointer, and device pointer
template <bool Async, typename Reducer, typename T, typename IndexType>
struct ReduceLoc_Data {
  //! union to hold either pointer to PinnedTally or poiter to value
  //  only use list before setup for device and only use val_ptr after
  union tally_u {
    PinnedTally<LocType<T, IndexType>>* list;
    LocType<T, IndexType> *val_ptr;
    tally_u(PinnedTally<LocType<T, IndexType>>* l) : list(l) {};
    tally_u(LocType<T, IndexType> *v_ptr) : val_ptr(v_ptr) {};
  };

  mutable T value;
  mutable IndexType index;
  tally_u tally_or_val_ptr;
  unsigned int* device_count;
  T *device;
  IndexType *deviceLoc;
  bool own_device_ptr;

  //! disallow default constructor
  ReduceLoc_Data() = delete;

  /*! \brief create from a default value and offload information
   *
   *  allocates PinnedTally to hold device values
   */
  explicit ReduceLoc_Data(T initValue, IndexType initIndex)
      : value{initValue},
        index{initIndex},
        tally_or_val_ptr{new PinnedTally<LocType<T, IndexType>>},
        device_count{nullptr},
        device{nullptr},
        deviceLoc{nullptr},
        own_device_ptr{false}
  {
  }

  RAJA_HOST_DEVICE
  ReduceLoc_Data(const ReduceLoc_Data &other)
      : value{Reducer::identity},
        index{-1},
        tally_or_val_ptr{other.tally_or_val_ptr},
        device_count{other.device_count},
        device{other.device},
        deviceLoc{other.deviceLoc},
        own_device_ptr{false}
  {
  }

  //! delete pinned tally
  RAJA_HOST_DEVICE
  RAJA_INLINE
  void destroy()
  {
#if !defined(__CUDA_ARCH__)
    delete tally_or_val_ptr.list; tally_or_val_ptr.list = nullptr;
#else
    cuda::LocType<T, IndexType> temp{value, index};
    if (impl::grid_reduceLoc<Reducer>(temp, device, deviceLoc,
                                      device_count)) {
      printf("writing %e\n", temp);
      tally_or_val_ptr.val_ptr[0] = temp;
    }
#endif
  }

  //! check and setup for device
  //  allocate device pointers and get a new result buffer from the pinned tally
  RAJA_HOST_DEVICE
  RAJA_INLINE
  bool setupForDevice(Offload_Info &info)
  {
    bool act = false;
#if !defined(__CUDA_ARCH__)
    act = !device && setupReducers();
    if (act) {
      dim3 gridDim = currentGridDim();
      size_t numBlocks = gridDim.x * gridDim.y * gridDim.z;
      device = device_mempool_type::getInstance().malloc<T>(numBlocks);
      deviceLoc = device_mempool_type::getInstance().malloc<IndexType>(numBlocks);
      device_count = device_zeroed_mempool_type::getInstance().malloc<unsigned int>(1);
      tally_or_val_ptr.val_ptr = tally_or_val_ptr.list->new_value(currentStream());
      own_device_ptr = true;
    }
#else
    act = impl::setup_grid_reduceLoc<Reducer>(device, deviceLoc,
                                              device_count);
#endif
    return act;
  }

  //! if own resources teardown device setup
  //  free device pointers
  RAJA_INLINE
  void teardownForDevice(Offload_Info&)
  {
    if(own_device_ptr) {
      device_mempool_type::getInstance().free(device);  device = nullptr;
      device_mempool_type::getInstance().free(deviceLoc);  deviceLoc = nullptr;
      device_zeroed_mempool_type::getInstance().free(device_count);  device_count = nullptr;
      tally_or_val_ptr.val_ptr = nullptr;
      own_device_ptr = false;
    }
  }

  //! transfers from the host to the device
  RAJA_INLINE
  void hostToDevice(Offload_Info &)
  {
  }

  //! transfers from the device to the host
  RAJA_INLINE
  void deviceToHost(Offload_Info &)
  {
    auto end = tally_or_val_ptr.list->streamEnd();
    for(auto s = tally_or_val_ptr.list->streamBegin(); s != end; ++s) {
      synchronize(*s);
    }
  }

  //! frees all data used
  //  frees all values in the pinned tally
  RAJA_INLINE
  void cleanup(Offload_Info &)
  {
    tally_or_val_ptr.list->free_list();
  }
};

//! Cuda Reduction entity -- generalize on reduction, and type
template <bool Async, typename Reducer, typename T>
struct Reduce {
  Reduce() = delete;

  //! create a reduce object
  //  the original object's parent is itself
  explicit Reduce(T init_val)
      : parent{this},
        info{},
        val(init_val)
  {
  }

  //! copy and on host attempt to setup for device
  RAJA_HOST_DEVICE
  Reduce(const Reduce & other)
#if !defined(__CUDA_ARCH__)
      : parent{other.parent},
#else
      : parent{&other},
#endif
        info(other.info),
        val(other.val)
  {
#if !defined(__CUDA_ARCH__)
    if (parent) {
      if (val.setupForDevice(info)) {
        parent = nullptr;
      }
    }
#else
    if (!parent->parent) {
      val.setupForDevice(info);
    }
#endif
  }

  //! apply reduction upon destruction and cleanup resources owned by this copy
  //  on device store in pinned buffer on host
  RAJA_HOST_DEVICE
  ~Reduce()
  {
#if !defined(__CUDA_ARCH__)
    if (parent == this) {
      val.destroy();
    } else if (parent) {
#if defined(RAJA_ENABLE_OPENMP) && defined(_OPENMP)
      lock_guard<omp::mutex> lock(val.tally_or_val_ptr.list->m_mutex);
#endif
      parent->reduce(val.value);
    } else {
      val.teardownForDevice(info);
    }
#else
    if (!parent->parent) {
      val.destroy();
    } else {
      parent->reduce(val.value);
    }
#endif
  }

  //! map result value back to host if not done already; return aggregate value
  operator T()
  {
    auto n = val.tally_or_val_ptr.list->begin();
    auto end = val.tally_or_val_ptr.list->end();
    if (n != end) {
      val.deviceToHost(info);
      for ( ; n != end; ++n) {
        Reducer{}(val.value, *n);
      }
      val.cleanup(info);
    }
    return val.value;
  }
  //! alias for operator T()
  T get() { return operator T(); }

  //! apply reduction
  RAJA_HOST_DEVICE
  Reduce &reduce(T rhsVal)
  {
    Reducer{}(val.value, rhsVal);
    return *this;
  }

  //! apply reduction (const version) -- still reduces internal values
  RAJA_HOST_DEVICE
  const Reduce &reduce(T rhsVal) const
  {
    using NonConst = typename std::remove_const<decltype(this)>::type;
    auto ptr = const_cast<NonConst>(this);
    Reducer{}(ptr->val.value,rhsVal);
    return *this;
  }

private:
  const Reduce<Async, Reducer, T>* parent;
  //! storage for offload information (host ID, device ID)
  cuda::Offload_Info info;
  //! storage for reduction data (host ptr, device ptr, value)
  cuda::Reduce_Data<Async, Reducer, T> val;
};


//! Cuda Reduction Atomic entity -- generalize on reduction, and type
template <bool Async, typename Reducer, typename T>
struct ReduceAtomic {
  ReduceAtomic() = delete;

  //! create a reduce object
  //  the original object's parent is itself
  explicit ReduceAtomic(T init_val)
      : parent{this},
        info{},
        val{init_val}
  {
  }

  //! copy and on host attempt to setup for device
  //  on device initialize device memory
  RAJA_HOST_DEVICE
  ReduceAtomic(const ReduceAtomic & other)
#if !defined(__CUDA_ARCH__)
      : parent{other.parent},
#else
      : parent{&other},
#endif
        info(other.info),
        val(other.val)
  {
#if !defined(__CUDA_ARCH__)
    if (parent) {
      if (val.setupForDevice(info)) {
        parent = nullptr;
      }
    }
#else
    if (!parent->parent) {
      val.setupForDevice(info);
    }
#endif
  }

  //! apply reduction upon destruction and cleanup resources owned by this copy
  //  on device store in pinned buffer on host
  RAJA_HOST_DEVICE
  ~ReduceAtomic()
  {
#if !defined(__CUDA_ARCH__)
    if (parent == this) {
      val.destroy();
    } else if (parent) {
#if defined(RAJA_ENABLE_OPENMP) && defined(_OPENMP)
      lock_guard<omp::mutex> lock(val.tally_or_val_ptr.list->m_mutex);
#endif
      parent->reduce(val.value);
    } else {
      val.teardownForDevice(info);
    }
#else
    if (!parent->parent) {
      val.destroy();
    } else {
      parent->reduce(val.value);
    }
#endif
  }

  //! map result value back to host if not done already; return aggregate value
  operator T()
  {
    auto n = val.tally_or_val_ptr.list->begin();
    auto end = val.tally_or_val_ptr.list->end();
    if (n != end) {
      val.deviceToHost(info);
      for ( ; n != end; ++n) {
        Reducer{}(val.value, *n);
      }
      val.cleanup(info);
    }
    return val.value;
  }
  //! alias for operator T()
  T get() { return operator T(); }

  //! apply reduction
  RAJA_HOST_DEVICE
  ReduceAtomic &reduce(T rhsVal)
  {
    Reducer{}(val.value, rhsVal);
    return *this;
  }

  //! apply reduction (const version) -- still reduces internal values
  RAJA_HOST_DEVICE
  const ReduceAtomic &reduce(T rhsVal) const
  {
    using NonConst = typename std::remove_const<decltype(this)>::type;
    auto ptr = const_cast<NonConst>(this);
    Reducer{}(ptr->val.value,rhsVal);
    return *this;
  }

private:
  const ReduceAtomic<Async, Reducer, T>* parent;
  //! storage for offload information (host ID, device ID)
  cuda::Offload_Info info;
  //! storage for reduction data (host ptr, device ptr, value)
  cuda::ReduceAtomic_Data<Async, Reducer, T> val;
};

//! Cuda Reduction Location entity -- generalize on reduction, and type
template <bool Async, typename Reducer, typename T, typename IndexType>
struct ReduceLoc {
  ReduceLoc() = delete;

  //! create a reduce object
  //  the original object's parent is itself
  explicit ReduceLoc(T init_val, IndexType init_loc)
      : parent{this},
        info{},
        val{init_val, init_loc}
  {
  }

  //! copy and on host attempt to setup for device
  RAJA_HOST_DEVICE
  ReduceLoc(const ReduceLoc & other)
#if !defined(__CUDA_ARCH__)
      : parent{other.parent},
#else
      : parent{&other},
#endif
        info(other.info),
        val(other.val)
  {
#if !defined(__CUDA_ARCH__)
    if (parent) {
      if (val.setupForDevice(info)) {
        parent = nullptr;
      }
    }
#else
    if (!parent->parent) {
      val.setupForDevice(info);
    }
#endif
  }

  //! apply reduction upon destruction and cleanup resources owned by this copy
  //  on device store in pinned buffer on host
  RAJA_HOST_DEVICE
  ~ReduceLoc()
  {
#if !defined(__CUDA_ARCH__)
    if (parent == this) {
      val.destroy();
    } else if (parent) {
#if defined(RAJA_ENABLE_OPENMP) && defined(_OPENMP)
      lock_guard<omp::mutex> lock(val.tally_or_val_ptr.list->m_mutex);
#endif
      parent->reduce(val.value, val.index);
    } else {
      val.teardownForDevice(info);
    }
#else
    if (!parent->parent) {
      val.destroy();
    } else {
      parent->reduce(val.value, val.index);
    }
#endif
  }

  //! map result value back to host if not done already; return aggregate value
  operator T()
  {
    auto n = val.tally_or_val_ptr.list->begin();
    auto end = val.tally_or_val_ptr.list->end();
    if (n != end) {
      val.deviceToHost(info);
      for ( ; n != end; ++n) {
        Reducer{}(val.value, val.index, (*n).val, (*n).idx);
      }
      val.cleanup(info);
    }
    return val.value;
  }
  //! alias for operator T()
  T get() { return operator T(); }

  //! map result value back to host if not done already; return aggregate location
  IndexType getLoc()
  {
    get();
    return val.index;
  }

  //! apply reduction
  RAJA_HOST_DEVICE
  ReduceLoc &reduce(T rhsVal, IndexType rhsLoc)
  {
    Reducer{}(val.value, val.index, rhsVal, rhsLoc);
    return *this;
  }

  //! apply reduction (const version) -- still reduces internal values
  RAJA_HOST_DEVICE
  const ReduceLoc &reduce(T rhsVal, IndexType rhsLoc) const
  {
    using NonConst = typename std::remove_const<decltype(this)>::type;
    auto ptr = const_cast<NonConst>(this);
    Reducer{}(ptr->val.value,ptr->val.index,rhsVal,rhsLoc);
    return *this;

  }

private:
  const ReduceLoc<Async, Reducer, T, IndexType>* parent;
  //! storage for offload information
  cuda::Offload_Info info;
  //! storage for reduction data for value and location
  cuda::ReduceLoc_Data<Async, Reducer, T, IndexType> val;
};

}  // end namespace cuda

//! specialization of ReduceSum for cuda_reduce
template <size_t BLOCK_SIZE, bool Async, typename T>
struct ReduceSum<cuda_reduce<BLOCK_SIZE, Async>, T>
    : public cuda::Reduce<Async, RAJA::reduce::sum<T>, T> {
  using self = ReduceSum<cuda_reduce<BLOCK_SIZE, Async>, T>;
  using parent = cuda::Reduce<Async, RAJA::reduce::sum<T>, T>;
  using parent::parent;
  //! enable operator+= for ReduceSum -- alias for reduce()
  RAJA_HOST_DEVICE
  self &operator+=(T rhsVal)
  {
    parent::reduce(rhsVal);
    return *this;
  }
  //! enable operator+= for ReduceSum -- alias for reduce()
  RAJA_HOST_DEVICE
  const self &operator+=(T rhsVal) const
  {
    parent::reduce(rhsVal);
    return *this;
  }
};

//! specialization of ReduceSum for cuda_reduce_atomic
template <size_t BLOCK_SIZE, bool Async, typename T>
struct ReduceSum<cuda_reduce_atomic<BLOCK_SIZE, Async>, T>
    : public cuda::ReduceAtomic<Async, RAJA::reduce::sum<T>, T> {
  using self = ReduceSum<cuda_reduce_atomic<BLOCK_SIZE, Async>, T>;
  using parent = cuda::ReduceAtomic<Async, RAJA::reduce::sum<T>, T>;
  using parent::parent;
  //! enable operator+= for ReduceSum -- alias for reduce()
  RAJA_HOST_DEVICE
  self &operator+=(T rhsVal)
  {
    parent::reduce(rhsVal);
    return *this;
  }
  //! enable operator+= for ReduceSum -- alias for reduce()
  RAJA_HOST_DEVICE
  const self &operator+=(T rhsVal) const
  {
    parent::reduce(rhsVal);
    return *this;
  }
};

//! specialization of ReduceMin for cuda_reduce_atomic
template <size_t BLOCK_SIZE, bool Async, typename T>
struct ReduceMin<cuda_reduce_atomic<BLOCK_SIZE, Async>, T>
    : public cuda::ReduceAtomic<Async, RAJA::reduce::min<T>, T> {
  using self = ReduceMin<cuda_reduce_atomic<BLOCK_SIZE, Async>, T>;
  using parent = cuda::ReduceAtomic<Async, RAJA::reduce::min<T>, T>;
  using parent::parent;
  //! enable min() for ReduceMin -- alias for reduce()
  RAJA_HOST_DEVICE
  self &min(T rhsVal)
  {
    parent::reduce(rhsVal);
    return *this;
  }
  //! enable min() for ReduceMin -- alias for reduce()
  RAJA_HOST_DEVICE
  const self &min(T rhsVal) const
  {
    parent::reduce(rhsVal);
    return *this;
  }
};

//! specialization of ReduceMin for cuda_reduce
template <size_t BLOCK_SIZE, bool Async, typename T>
struct ReduceMin<cuda_reduce<BLOCK_SIZE, Async>, T>
    : public cuda::Reduce<Async, RAJA::reduce::min<T>, T> {
  using self = ReduceMin<cuda_reduce<BLOCK_SIZE, Async>, T>;
  using parent = cuda::Reduce<Async, RAJA::reduce::min<T>, T>;
  using parent::parent;
  //! enable min() for ReduceMin -- alias for reduce()
  RAJA_HOST_DEVICE
  self &min(T rhsVal)
  {
    parent::reduce(rhsVal);
    return *this;
  }
  //! enable min() for ReduceMin -- alias for reduce()
  RAJA_HOST_DEVICE
  const self &min(T rhsVal) const
  {
    parent::reduce(rhsVal);
    return *this;
  }
};

//! specialization of ReduceMax for cuda_reduce_atomic
template <size_t BLOCK_SIZE, bool Async, typename T>
struct ReduceMax<cuda_reduce_atomic<BLOCK_SIZE, Async>, T>
    : public cuda::ReduceAtomic<Async, RAJA::reduce::max<T>, T> {
  using self = ReduceMax<cuda_reduce_atomic<BLOCK_SIZE, Async>, T>;
  using parent = cuda::ReduceAtomic<Async, RAJA::reduce::max<T>, T>;
  using parent::parent;
  //! enable max() for ReduceMax -- alias for reduce()
  RAJA_HOST_DEVICE
  self &max(T rhsVal)
  {
    parent::reduce(rhsVal);
    return *this;
  }
  //! enable max() for ReduceMax -- alias for reduce()
  RAJA_HOST_DEVICE
  const self &max(T rhsVal) const
  {
    parent::reduce(rhsVal);
    return *this;
  }
};

//! specialization of ReduceMax for cuda_reduce
template <size_t BLOCK_SIZE, bool Async, typename T>
struct ReduceMax<cuda_reduce<BLOCK_SIZE, Async>, T>
    : public cuda::Reduce<Async, RAJA::reduce::max<T>, T> {
  using self = ReduceMax<cuda_reduce<BLOCK_SIZE, Async>, T>;
  using parent = cuda::Reduce<Async, RAJA::reduce::max<T>, T>;
  using parent::parent;
  //! enable max() for ReduceMax -- alias for reduce()
  RAJA_HOST_DEVICE
  self &max(T rhsVal)
  {
    parent::reduce(rhsVal);
    return *this;
  }
  //! enable max() for ReduceMax -- alias for reduce()
  RAJA_HOST_DEVICE
  const self &max(T rhsVal) const
  {
    parent::reduce(rhsVal);
    return *this;
  }
};

//! specialization of ReduceMinLoc for cuda_reduce
template <size_t BLOCK_SIZE, bool Async, typename T>
struct ReduceMinLoc<cuda_reduce<BLOCK_SIZE, Async>, T>
    : public cuda::ReduceLoc<Async, RAJA::reduce::minloc<T, Index_type>, T, Index_type> {
  using self = ReduceMinLoc<cuda_reduce<BLOCK_SIZE, Async>, T>;
  using parent =
    cuda::ReduceLoc<Async, RAJA::reduce::minloc<T, Index_type>, T, Index_type>;
  using parent::parent;
  //! enable minloc() for ReduceMinLoc -- alias for reduce()
  RAJA_HOST_DEVICE
  self &minloc(T rhsVal, Index_type rhsLoc)
  {
    parent::reduce(rhsVal, rhsLoc);
    return *this;
  }
  //! enable minloc() for ReduceMinLoc -- alias for reduce()
  RAJA_HOST_DEVICE
  const self &minloc(T rhsVal, Index_type rhsLoc) const
  {
    parent::reduce(rhsVal, rhsLoc);
    return *this;
  }
};

//! specialization of ReduceMaxLoc for cuda_reduce
template <size_t BLOCK_SIZE, bool Async, typename T>
struct ReduceMaxLoc<cuda_reduce<BLOCK_SIZE, Async>, T>
    : public cuda::ReduceLoc<Async, RAJA::reduce::maxloc<T, Index_type>, T, Index_type> {
  using self = ReduceMaxLoc<cuda_reduce<BLOCK_SIZE, Async>, T>;
  using parent =
    cuda::ReduceLoc<Async, RAJA::reduce::maxloc<T, Index_type>, T, Index_type>;
  using parent::parent;
  //! enable maxloc() for ReduceMaxLoc -- alias for reduce()
  RAJA_HOST_DEVICE
  self &maxloc(T rhsVal, Index_type rhsLoc)
  {
    parent::reduce(rhsVal, rhsLoc);
    return *this;
  }
  //! enable maxloc() for ReduceMaxLoc -- alias for reduce()
  RAJA_HOST_DEVICE
  const self &maxloc(T rhsVal, Index_type rhsLoc) const
  {
    parent::reduce(rhsVal, rhsLoc);
    return *this;
  }
};

}  // closing brace for RAJA namespace

#endif  // closing endif for RAJA_ENABLE_CUDA guard

#endif  // closing endif for header file include guard
