#include <random>
#include <assert.h>
#include <iostream>

#include "nvcomp/lz4.h"

/* 
  To build, execute
  
  mkdir build
  cd build
  cmake -DBUILD_EXAMPLES=ON ..
  make -j

  To execute, 
  bin/low_level_quickstart_example
*/

void execute_example(char* input_data, const size_t in_bytes)
{
  cudaStream_t stream;
  cudaStreamCreate(&stream);

  // First, initialize the data on the host.

  // compute chunk sizes
  size_t* host_uncompressed_bytes;
  const size_t chunk_size = 65536;
  const size_t batch_size = (in_bytes + chunk_size - 1) / chunk_size;

  char* device_input_data;
  cudaMalloc((void**)&device_input_data, in_bytes);
  cudaMemcpyAsync(device_input_data, input_data, in_bytes, cudaMemcpyHostToDevice, stream);

  cudaMallocHost((void**)&host_uncompressed_bytes, sizeof(size_t)*batch_size);
  for (size_t i = 0; i < batch_size; ++i) {
    if (i + 1 < batch_size) {
      host_uncompressed_bytes[i] = chunk_size;
    } else {
      // last chunk may be smaller
      host_uncompressed_bytes[i] = in_bytes - (chunk_size*i);
    }
  }

  // Setup an array of pointers to the start of each chunk
  void ** host_uncompressed_ptrs;
  cudaMallocHost((void**)&host_uncompressed_ptrs, sizeof(size_t)*batch_size);
  for (size_t ix_chunk = 0; ix_chunk < batch_size; ++ix_chunk) {
    host_uncompressed_ptrs[ix_chunk] = device_input_data + chunk_size*ix_chunk;
  }

  size_t* device_uncompressed_bytes;
  void ** device_uncompressed_ptrs;
  cudaMalloc((void**)&device_uncompressed_bytes, sizeof(size_t) * batch_size);
  cudaMalloc((void**)&device_uncompressed_ptrs, sizeof(size_t) * batch_size);
  
  cudaMemcpyAsync(device_uncompressed_bytes, host_uncompressed_bytes, sizeof(size_t) * batch_size, cudaMemcpyHostToDevice, stream);
  cudaMemcpyAsync(device_uncompressed_ptrs, host_uncompressed_ptrs, sizeof(size_t) * batch_size, cudaMemcpyHostToDevice, stream);

  // Then we need to allocate the temporary workspace and output space needed by the compressor.
  size_t temp_bytes;
  nvcompBatchedLZ4CompressGetTempSize(batch_size, chunk_size, nvcompBatchedLZ4DefaultOpts, &temp_bytes);
  void* device_temp_ptr;
  cudaMalloc(&device_temp_ptr, temp_bytes);

  // get the maxmimum output size for each chunk
  size_t max_out_bytes;
  nvcompBatchedLZ4CompressGetMaxOutputChunkSize(chunk_size, nvcompBatchedLZ4DefaultOpts, &max_out_bytes);

  // Next, allocate output space on the device
  void ** host_compressed_ptrs;
  cudaMallocHost((void**)&host_compressed_ptrs, sizeof(size_t) * batch_size);
  for(size_t ix_chunk = 0; ix_chunk < batch_size; ++ix_chunk) {
      cudaMalloc(&host_compressed_ptrs[ix_chunk], max_out_bytes);
  }

  void** device_compressed_ptrs;
  cudaMalloc((void**)&device_compressed_ptrs, sizeof(size_t) * batch_size);
  cudaMemcpyAsync(
      device_compressed_ptrs, host_compressed_ptrs, 
      sizeof(size_t) * batch_size,cudaMemcpyHostToDevice, stream);

  // allocate space for compressed chunk sizes to be written to
  size_t * device_compressed_bytes;
  cudaMalloc((void**)&device_compressed_bytes, sizeof(size_t) * batch_size);

  // And finally, call the API to compress the data
  nvcompStatus_t comp_res = nvcompBatchedLZ4CompressAsync(  
      device_uncompressed_ptrs,    
      device_uncompressed_bytes,  
      chunk_size, // The maximum chunk size  
      batch_size,  
      device_temp_ptr,  
      temp_bytes,  
      device_compressed_ptrs,  
      device_compressed_bytes,  
      nvcompBatchedLZ4DefaultOpts,  
      stream);

  if (comp_res != nvcompSuccess)
  {
    std::cerr << "Failed compression!" << std::endl;
    assert(comp_res == nvcompSuccess);
  }

  // Decompression can be similarly performed on a batch of multiple compressed input chunks. 
  // As no metadata is stored with the compressed data, chunks can be re-arranged as well as decompressed 
  // with other chunks that originally were not compressed in the same batch.

  // If we didn't have the uncompressed sizes, we'd need to compute this information here. 
  // We demonstrate how to do this.
  nvcompBatchedLZ4GetDecompressSizeAsync(
      device_compressed_ptrs,
      device_compressed_bytes,
      device_uncompressed_bytes,
      batch_size,
      stream);

  // Next, allocate the temporary buffer 
  size_t decomp_temp_bytes;
  nvcompBatchedLZ4DecompressGetTempSize(batch_size, chunk_size, &decomp_temp_bytes);
  void * device_decomp_temp;
  cudaMalloc(&device_decomp_temp, decomp_temp_bytes);

  // allocate statuses
  nvcompStatus_t* device_statuses;
  cudaMalloc(&device_statuses, sizeof(nvcompStatus_t)*batch_size);

  // Also allocate an array to store the actual_uncompressed_bytes.
  // Note that we could use nullptr for this. We already have the 
  // actual sizes computed during the call to nvcompBatchedLZ4GetDecompressSizeAsync.
  size_t* device_actual_uncompressed_bytes;
  cudaMalloc(&device_actual_uncompressed_bytes, sizeof(size_t)*batch_size);

  // And finally, call the decompression routine.
  // This decompresses each input, device_compressed_ptrs[i], and places the decompressed
  // result in the corresponding output list, device_uncompressed_ptrs[i]. It also writes
  // the size of the uncompressed data to device_uncompressed_bytes[i].
  nvcompStatus_t decomp_res = nvcompBatchedLZ4DecompressAsync(
      device_compressed_ptrs, 
      device_compressed_bytes, 
      device_uncompressed_bytes, 
      device_actual_uncompressed_bytes, 
      batch_size,
      device_decomp_temp, 
      decomp_temp_bytes, 
      device_uncompressed_ptrs, 
      device_statuses, 
      stream);
  
  if (decomp_res != nvcompSuccess)
  {
    std::cerr << "Failed compression!" << std::endl;
    assert(decomp_res == nvcompSuccess);
  }

  cudaStreamSynchronize(stream);
}

int main()
{
  // Initialize a random array of chars
  const size_t in_bytes = 1e6;
  char* uncompressed_data;
  
  cudaMallocHost((void**)&uncompressed_data, in_bytes);
  
  std::mt19937 random_gen(42);
  std::uniform_int_distribution<uint8_t> uniform_dist(0, 255);
  for (size_t ix = 0; ix < in_bytes; ++ix) {
    uncompressed_data[ix] = uniform_dist(random_gen);
  }
  
  execute_example(uncompressed_data, in_bytes);
  return 0;
  
}