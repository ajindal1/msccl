/*************************************************************************
 * Copyright (c) 2015-2020, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "devcomm.h"
#include "primitives.h"
#include "collectives.h"

#define SCKL_MAX_ITER 65536

// flags are a 3-tuple of (workindex, gridoffset_iter, step) and it follows a lexicographical order. a threadblock is ahead of another iff its flag is ahead 
#define COMPUTE_FLAG(__WORKINDEX__,__GRIDOFFSET_ITER__,__STEP__) \
   SCKL_MAX_ITER*SCKL_MAX_NUM_STEPS*(int64_t)__WORKINDEX__ + ((int64_t)__GRIDOFFSET_ITER__ * SCKL_MAX_NUM_STEPS + (int64_t)__STEP__)

template<typename T, typename PRIMS_WRAPPER>
class SCKLFunction {
  public:
    __device__ void run(struct ncclWorkElem* args) {
      struct ncclDevComm* comm = args->comm;
      struct scklAlgorithm* scklAlgo = &comm->scklAlgo;
      const int tid = threadIdx.x;
      const int sync_tid = args->nThreads-1; // last thread is most likely not doing anthing and used for sckl cross thread synchronization
      const int bid = blockIdx.x;
      const int scklNBlocks = scklAlgo->nBlocks;
      const int rscklbid = bid % scklNBlocks; // bid within a sckl algo
      const int scklIndex = bid / scklNBlocks; // which instance of sckl algo
      const int nScklInstnaces = gridDim.x / scklAlgo->nBlocks; // number of sckl aglos
      struct scklThreadBlock* scklTB = &scklAlgo->scklTB[rscklbid];
      const int channelId = scklIndex * scklAlgo->nChannels + scklTB->channelId;
      struct ncclChannel* channel = comm->channels+channelId;

      // Compute pointers
      T * thisInput = (T*)args->sendbuff;
      T * thisOutput = (T*)args->recvbuff;
      int recvPeer = scklTB->recvpeer;
      int sendPeer = scklTB->sendpeer;

      PRIMS_WRAPPER prims{args, tid, &recvPeer, &sendPeer, thisOutput, channel};

      const int nranks = comm->nRanks;
      const ssize_t loopSize = (ssize_t)prims.chunkSize*nScklInstnaces;
      const ssize_t size = args->coll.count;
      const ssize_t sizePerScklChunk = (size*nranks)/scklAlgo->nchunksPerLoop;

      int chunkEffectiveSize = prims.chunkEffectiveSize;
      // TODO: add this to the info
      int scclMaxAllowedCount = max((int)(chunkEffectiveSize / DIVUP(size*nranks, (size_t)(scklAlgo->nchunksPerLoop * nScklInstnaces))),1);
      // if (tid == 0 && bid == 0)
      //   printf("scclMaxAllowedCount %d chunkEffectiveSize %d\n", scclMaxAllowedCount, chunkEffectiveSize);

      // sckl flags all start out with 0. this is used as a part of the flag to make sure different work items deal with different synchronization flags
      // this still needs more work. when we make a way around the queue, the flag might have been set to undesired values. will be fixed in subsequent versions.
      const int workIndex = args->index+1;
      volatile struct scklFlag* scklFlags = comm->scklFlags;

      for (ssize_t gridOffset = 0, iter = 0; gridOffset < sizePerScklChunk; gridOffset += loopSize, iter++) {
        size_t chunkOffset = prims.initIter(sizePerScklChunk, gridOffset, nScklInstnaces, scklIndex);
        ssize_t srcoffset, dstoffset;
        T* srcPointer, * dstPointer;
        for (int i = 0; i < scklTB->nsteps; i++){
          struct scklTransfer* sckltran = &scklTB->transfers[i];
          // if (sckltran->type == SCKL_NO_OP) continue;
          // first wait if there is a dependence
          int8_t dependentBid = sckltran->dependentBid + scklIndex * scklNBlocks;
          int8_t dependentStep = sckltran->dependentStep;
          if (sckltran->dependentBid >= 0){
              if (tid == sync_tid){
                uint64_t goalFlag = COMPUTE_FLAG(workIndex, iter, dependentStep);
                while ((scklFlags + dependentBid)->flag < goalFlag){};
              }
              __syncthreads();
          }

          // if (tid == 0)
          //   printf("1111 %d %d %d %d\n", workIndex, (int) iter, bid, i);
          srcPointer = (sckltran->srcbuffer == SCKL_INPUT_BUFFER) ? thisInput : thisOutput;
          dstPointer = (sckltran->dstbuffer == SCKL_INPUT_BUFFER) ? thisInput : thisOutput;
          int count = sckltran->count;
          for (int c = 0; c < count; c += scclMaxAllowedCount) {
            srcoffset = chunkOffset + (ssize_t) (sckltran->srcoffset+c) * sizePerScklChunk;
            dstoffset = chunkOffset + (ssize_t) (sckltran->dstoffset+c) * sizePerScklChunk;
            int thisCount = min(scclMaxAllowedCount, count-c);
            switch (sckltran->type) {
              case SCKL_SEND:
                prims.send(srcPointer + srcoffset, dstoffset, thisCount);
                break;
              case SCKL_RECV:
                prims.recv(dstPointer + dstoffset, dstoffset, thisCount);
                break;
              case SCKL_RECV_COPY_SEND:
                prims.recvCopySend(dstPointer + dstoffset, dstoffset, thisCount);
                break;
              case SCKL_RECV_REDUCE_SEND:
                prims.recvReduceSend(srcPointer + srcoffset, thisCount);
                break;
              case SCKL_RECV_REDUCE_COPY:
                prims.recvReduceCopy(srcPointer + srcoffset, dstPointer + dstoffset, thisCount);
                break;
              case SCKL_NO_OP:
                break;
              default:
                return;
            }
          }
          // if (tid == 0)
          //   printf("2222 %d %d %d %d\n", workIndex, (int) iter, bid, i);
          if (tid == sync_tid && sckltran->has_dependence){
            __threadfence();
            uint64_t curFlag = COMPUTE_FLAG(workIndex, iter, i);
            scklFlags[bid].flag = curFlag;
            __threadfence();
          }
        }
      }
      if (tid == 0)
        printf("bid %d workIndex %d is done\n", bid, workIndex);
    }
};

template<class FUNC, typename T, int UNROLL>
struct SimpleWrapper {
  const int nthreads;
  const int stepSize;
  const int chunkSize;
  int chunkEffectiveSize;

  ncclPrimitives<UNROLL, SCKL_CHUNKSTEPS/SCKL_SLICESTEPS, SCKL_SLICESTEPS, T, 1, 1, 1, FUNC> prims;

  int nelem;

  __device__ SimpleWrapper(struct ncclWorkElem* args, int tid, int* recvPeer, int* sendPeer, T * thisOutput, struct ncclChannel* channel)
    : nthreads(args->nThreads-WARP_SIZE),
      stepSize(args->comm->buffSizes[NCCL_PROTO_SIMPLE] / (sizeof(T)*NCCL_STEPS)),
      chunkSize(stepSize * SCKL_CHUNKSTEPS), chunkEffectiveSize(chunkSize),
      prims(tid, nthreads, recvPeer, sendPeer, thisOutput, stepSize, channel, args->comm, ncclShmem->ptrs, 0) {}

  __device__ size_t initIter(ssize_t sizePerScklChunk, ssize_t gridOffset, int nScklInstnaces, int scklIndex) {
    int realChunkSize = min(chunkSize, DIVUP(sizePerScklChunk-gridOffset,nScklInstnaces));
    ALIGN_SIZE(realChunkSize, nthreads*sizeof(uint64_t)/sizeof(T));
    ssize_t chunkOffset = gridOffset + scklIndex*realChunkSize;
    nelem = min(realChunkSize, sizePerScklChunk-chunkOffset);
    return chunkOffset;
  }

  __device__ void send(T * chunkPointer, ssize_t dstoffset, int count) {
    prims.directSend(chunkPointer, dstoffset, nelem*count);
  }

  __device__ void recv(T * chunkPointer, ssize_t dstoffset, int count) {
    prims.directRecv(chunkPointer, dstoffset, nelem*count);
  }

  __device__ void recvCopySend(T * chunkPointer, ssize_t dstoffset, int count) {
    prims.directRecvCopySend(chunkPointer, dstoffset, nelem*count);
  }
  
  __device__ void recvReduceSend(T * chunkPointer, int count) {
    prims.recvReduceSend(chunkPointer, nelem*count);
  }

  __device__ void recvReduceCopy(T * srcChunkPointer, T * dstChunkPointer, int count) {
    prims.recvReduceCopy(srcChunkPointer, dstChunkPointer, nelem*count);
  }
};

template<class FUNC, typename T, int UNROLL>
class SCKLFunctionSimple : public SCKLFunction<T, SimpleWrapper<FUNC, T, UNROLL>> {};

#include "prims_ll128.h"
template<class FUNC, typename T>
struct LL128Wrapper {
  const int stepSize;
  ssize_t chunkSize;
  const ssize_t minChunkSize;
  int chunkEffectiveSize;
  ncclLL128Primitives<T, FUNC, 1, 1> prims;
  int nelem;

  __device__ LL128Wrapper(struct ncclWorkElem* args, int tid, int* recvPeer, int* sendPeer, T * thisOutput, struct ncclChannel* channel)
    : stepSize(args->comm->buffSizes[NCCL_PROTO_LL128] / (sizeof(uint64_t)*NCCL_STEPS)),
      chunkSize(stepSize*NCCL_LL128_DATAELEMS*sizeof(uint64_t) / (NCCL_LL128_LINEELEMS*sizeof(T))),
      minChunkSize((NCCL_LL128_SHMEM_ELEMS_PER_THREAD*args->nThreads*NCCL_LL128_DATAELEMS*sizeof(uint64_t))/(NCCL_LL128_LINEELEMS*sizeof(T))/2), chunkEffectiveSize(chunkSize),
      prims(tid, args->nThreads, recvPeer, sendPeer, stepSize, channel, args->comm) {}

  __device__ size_t initIter(ssize_t sizePerScklChunk, ssize_t gridOffset, int nScklInstnaces, int scklIndex) {
    chunkSize = min(chunkSize, DIVUP(sizePerScklChunk-gridOffset,nScklInstnaces*minChunkSize)*minChunkSize);
    ssize_t chunkOffset = gridOffset + scklIndex*chunkSize;
    nelem = min(chunkSize, sizePerScklChunk-chunkOffset);
    return chunkOffset;
  }

  __device__ void send(T * chunkPointer, ssize_t dstoffset, int count) {
    prims.send(chunkPointer, nelem*count);
  }

  __device__ void recv(T * chunkPointer, ssize_t dstoffset, int count) {
    prims.recv(chunkPointer, nelem*count);
  }

  __device__ void recvCopySend(T * chunkPointer, ssize_t dstoffset, int count) {
    prims.recvCopySend(chunkPointer, nelem*count);
  }

  __device__ void recvReduceSend(T * chunkPointer, int count) {
    prims.recvReduceSend(chunkPointer, nelem*count);
  }

  __device__ void recvReduceCopy(T * srcChunkPointer, T * dstChunkPointer, int count) {
    prims.recvReduceCopy(srcChunkPointer, dstChunkPointer, nelem*count);
  }  
};

template<class FUNC, typename T, int UNROLL>
class SCKLFunctionLL128 : public SCKLFunction<T, LL128Wrapper<FUNC, T>> {};

template<class FUNC, typename T>
struct LLWrapper {
  const int stepLines;
  const ssize_t chunkSize;
  int chunkEffectiveSize;
  ncclLLPrimitives<T, FUNC, 1, 1> prims;
  int nelem;

  __device__ LLWrapper(struct ncclWorkElem* args, int tid, int* recvPeer, int* sendPeer, T * thisOutput, struct ncclChannel* channel)
    : stepLines(args->comm->buffSizes[NCCL_PROTO_LL] / (sizeof(union ncclLLFifoLine)*NCCL_STEPS)),
      chunkSize(stepLines * sizeof(uint64_t) / sizeof(T)), chunkEffectiveSize((int)chunkSize),
      prims(tid, args->nThreads, recvPeer, sendPeer, stepLines, channel, args->comm) {}

  // TODO: nScklInstances should be used. Buggy!
  __device__ size_t initIter(ssize_t sizePerScklChunk, ssize_t gridOffset, int nScklInstnaces, int scklIndex) {
    ssize_t chunkOffset = gridOffset + scklIndex*chunkSize;
    nelem = min(chunkSize, sizePerScklChunk-chunkOffset);
    return chunkOffset;
  }

  __device__ void send(T * chunkPointer, ssize_t dstoffset, int count) {
    prims.send(chunkPointer, nelem*count);
  }

  __device__ void recv(T * chunkPointer, ssize_t dstoffset, int count) {
    prims.recv(chunkPointer, nelem*count);
  }

  __device__ void recvCopySend(T * chunkPointer, ssize_t dstoffset, int count) {
    prims.recvCopySend(chunkPointer, nelem*count);
  }

  __device__ void recvReduceSend(T * chunkPointer, int count) {
    prims.recvReduceSend(chunkPointer, nelem*count);
  }

  __device__ void recvReduceCopy(T * srcChunkPointer, T * dstChunkPointer, int count) {
    prims.recvReduceCopy(srcChunkPointer, dstChunkPointer, nelem*count);
  }  
};

template<class FUNC, typename T, int UNROLL>
class SCKLFunctionLL : public SCKLFunction<T, LLWrapper<FUNC, T>> {};
