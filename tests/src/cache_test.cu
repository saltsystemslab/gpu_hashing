/*
 * ============================================================================
 *
 *        Authors:  
 *                  Hunter McCoy <hjmccoy@lbl.gov
 *
 * ============================================================================
 */





#include <gallatin/allocators/global_allocator.cuh>

#include <gallatin/allocators/timer.cuh>

#include <warpcore/single_value_hash_table.cuh>
#include <gallatin/allocators/timer.cuh>

#include <bght/p2bht.hpp>

#include <hashing_project/cache.cuh>

#include <stdio.h>
#include <iostream>
#include <assert.h>
#include <chrono>
#include <openssl/rand.h>


#include <hashing_project/table_wrappers/p2_wrapper.cuh>
#include <hashing_project/table_wrappers/dummy_ht.cuh>
#include <hashing_project/table_wrappers/iht_wrapper.cuh>

#include <cooperative_groups.h>

namespace cg = cooperative_groups;

using namespace gallatin::allocators;


#if GALLATIN_DEBUG_PRINTS
   #define TEST_BLOCK_SIZE 256
#else
   #define TEST_BLOCK_SIZE 256
#endif


template <typename T>
__host__ T * generate_data(uint64_t nitems){


   //malloc space

   T * vals;

   cudaMallocHost((void **)&vals, sizeof(T)*nitems);


   //          100,000,000
   uint64_t cap = 100000000ULL;

   for (uint64_t to_fill = 0; to_fill < nitems; to_fill+=0){

      uint64_t togen = (nitems - to_fill > cap) ? cap : nitems - to_fill;


      RAND_bytes((unsigned char *) (vals + to_fill), togen * sizeof(T));



      to_fill += togen;

      //printf("Generated %llu/%llu\n", to_fill, nitems);

   }

   printf("Generation done\n");
   return vals;
}


template <typename cache_type, uint tile_size>
__global__ void test_cache_kernel(cache_type * cache, uint64_t n_indices, uint64_t * access_pattern, uint64_t n_ops){


   auto thread_block = cg::this_thread_block();

   cg::thread_block_tile<tile_size> my_tile = cg::tiled_partition<tile_size>(thread_block);


   uint64_t tid = gallatin::utils::get_tile_tid(my_tile);

   if (tid >= n_ops) return;

   uint64_t my_access = access_pattern[tid] % n_indices;

   //if (my_access == 0) my_access = 1;

   cache->increment_index(my_tile, my_access);

   //if (my_tile.thread_rank() == 0) printf("Done with %lu\n", tid);

   //cache->write_back_host<tile_size>(my_tile, my_access, 1);


}

//pull from blocks
//this kernel tests correctness, and outputs misses in a counter.
//works on actual pointers instead of uint64_t
//The correctness check is done by treating each allocation as a uint64_t and writing the tid
// if TID is not what is expected, we know that a double malloc has occurred.
template <template<typename, typename, uint> typename hash_table, uint tile_size>
__host__ void cache_test(uint64_t host_items, uint64_t cache_items, uint64_t n_ops){


   using cache_type = hashing_project::cache<hash_table, tile_size>;

   cache_type * cache = cache_type::generate_on_device(host_items, cache_items);


   uint64_t * access_data = generate_data<uint64_t>(n_ops);

   uint64_t * dev_data = gallatin::utils::get_device_version<uint64_t>(n_ops);

   cudaMemcpy(dev_data, access_data, sizeof(uint64_t)*n_ops, cudaMemcpyHostToDevice);

   //uint64_t * dev_data = gallatin::utils::copy_to_device<uint64_t>(access_data, n_ops);

   cudaDeviceSynchronize();

   gallatin::utils::timer cache_timing;

   test_cache_kernel<cache_type, tile_size><<<(n_ops*tile_size -1)/256+1,256>>>(cache, host_items, dev_data, n_ops);

   cache_timing.sync_end();

   cache_timing.print_throughput("Modified", n_ops);

   cudaDeviceSynchronize();

   cache->check_compared_to_host(access_data, n_ops, host_items);

   //cudaFree(access_data);

}


int main(int argc, char** argv) {

   uint64_t host_items;

   uint64_t cache_items;

   uint64_t n_ops;


   if (argc < 2){
      host_items = 1000000;
   } else {
      host_items = std::stoull(argv[1]);
   }

   if (argc < 3){
      cache_items = 1000;
   } else {
      cache_items = std::stoull(argv[2]);
   }

   if (argc < 4){
      n_ops = 1000;
   } else {
      n_ops = std::stoull(argv[3]);
   }
   
   cache_test<hashing_project::wrappers::iht_wrapper, 32>(host_items, cache_items, n_ops);

   cache_test<hashing_project::wrappers::p2_wrapper, 32>(host_items, cache_items, n_ops);



   cudaDeviceReset();
   return 0;

}