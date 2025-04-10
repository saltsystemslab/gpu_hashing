/*
 *   Copyright 2021 The Regents of the University of California, Davis
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once
#include <bght/allocator.hpp>
#include <bght/detail/cuda_helpers.cuh>
#include <bght/detail/kernels.cuh>
#include <bght/hash_functions.hpp>
#include <bght/pair.cuh>
#include <cuda/atomic>
#include <cuda/std/utility>
#include <memory>

namespace bght {

/**
 * @brief P2BHT P2BHT (power-of-two bucketed hash table) is an associative static GPU hash
 * table that contains key-value pairs with unique keys. The hash table is an open
 * addressing hash table based on the power-of-two hashing to balance loads between
 * buckets (bucketed and using two hash functions).
 *
 * @tparam Key Type for the hash map key
 * @tparam T Type for the mapped value
 * @tparam Hash Unary function object class that defines the hash function. The function
 * must have an `initialize_hf` specialization to initialize the hash function using a
 * random number generator
 * @tparam KeyEqual Binary function object class that compares two keys
 * @tparam Allocator The allocator to use for allocating GPU device memory
 * @tparam B Bucket size for the hash table
 */
template <class Key,
          class T,
          class Hash = bght::MurmurHash3_32<Key>,
          class KeyEqual = bght::equal_to<Key>,
          cuda::thread_scope Scope = cuda::thread_scope_device,
          class Allocator = bght::cuda_allocator<char>,
          int B = 16>
struct p2bht {
  using value_type = pair<Key, T>;
  using key_type = Key;
  using mapped_type = T;
  using atomic_pair_type = cuda::atomic<value_type, Scope>;
  using allocator_type = Allocator;
  using hasher = Hash;
  using size_type = std::size_t;

  using atomic_pair_allocator_type =
      typename std::allocator_traits<Allocator>::rebind_alloc<atomic_pair_type>;
  using pool_allocator_type =
      typename std::allocator_traits<Allocator>::rebind_alloc<bool>;
  using size_type_allocator_type =
      typename std::allocator_traits<Allocator>::rebind_alloc<size_type>;


  static constexpr auto bucket_size = B;
  using key_equal = KeyEqual;

  using tile_type = cg::thread_block_tile<B>;

  /**
   * @brief Constructs the hash table with the specified capacity and uses the specified
   * sentinel key and value to define a sentinel pair.
   *
   * @param capacity The number of slots to use in the hash table. If the capacity is not
   * multiple of the bucket size, it will be rounded
   * @param sentinel_key A reserved sentinel key that defines an empty key
   * @param sentinel_value A reserved sentinel value that defines an empty value
   * @param allocator The allocator to use for allocating GPU device memory
   */
  p2bht(std::size_t capacity,
        Key sentinel_key,
        Key tombstone_key_,
        T sentinel_value,
        Allocator const& allocator = Allocator{});


  static __host__ p2bht * generate_on_device(std::size_t capacity,
    Key empty_key_sentinel,
    Key empty_key_tombstone,
    T empty_value_sentinel,
    Allocator const& allocator = Allocator{});

  /**
   * @brief A shallow-copy constructor
   */
  p2bht(const p2bht& other);
  /**
   * @brief Move constructor is currently deleted
   */
  p2bht(p2bht&&) = delete;
  /**
   * @brief The assignment operator is currently deleted
   */
  p2bht& operator=(const p2bht&) = delete;
  /**
   * @brief The move assignment operator is currently deleted
   */
  p2bht& operator=(p2bht&&) = delete;
  /**
   * @brief Destructor that destroys the hash map and deallocate memory if no copies exist
   */
  ~p2bht();
  /**
   * @brief Clears the hash map and resets all slots
   */
  void clear();

  /**
   * @brief Host-side API for inserting all pairs defined by the input argument iterators.
   * All keys in the range must be unique and must not exist in the hash table.
   * @tparam InputIt Device-side iterator that can be converted to `value_type`.
   * @param first An iterator defining the beginning of the input pairs to insert
   * @param last  An iterator defining the end of the input pairs to insert
   * @param stream  A CUDA stream where the insertion operation will take place
   * @return A boolean indicating success (true) or failure (false) of the insertion
   * operation.
   */
  template <typename InputIt>
  bool insert(InputIt first, InputIt last, cudaStream_t stream = 0);

  /**
   * @brief Host-side API for finding all keys defined by the input argument iterators.
   * @tparam InputIt  Device-side iterator that can be converted to `key_type`
   * @tparam OutputIt Device-side iterator that can be converted to `mapped_type`
   * @param first An iterator defining the beginning of the input keys to find
   * @param last An iterator defining the end of the input keys to find
   * @param output_begin An iterator defining the beginning of the output buffer to store
   * the results into. The size of the buffer must match the number of queries defined by
   * the input iterators.
   * @param stream  A CUDA stream where the insertion operation will take place
   */
  template <typename InputIt, typename OutputIt>
  void find(InputIt first, InputIt last, OutputIt output_begin, cudaStream_t stream = 0);



__device__ bool find_by_reference(tile_type const& tile, key_type const& key, mapped_type &value);

  /**
   * @brief Device-side cooperative insertion API that inserts a single pair into the hash
   * map.
   * @tparam tile_type A cooperative group tile with a size that must match the bucket
   * size of the hash map (i.e., `bucket_size`). It must support the tile-wide intrinsics
   * `ballot`, `shfl`
   * @param pair A key-value pair to insert into the hash map. The pair must be the same
   * for all threads in the  cooperative group tile
   * @param tile  The cooperative group tile
   * @return A boolean indicating success (true) or failure (false) of the insertion
   * operation.
   */

  __device__ bool insert(value_type const& pair, tile_type const& tile);



  /**
   * @brief Device-side cooperative upsert API that inserts a single pair into the hash
   * map if it does not exist. Replaces existing pair IFF if one exists in the table.
   * @tparam tile_type A cooperative group tile with a size that must match the bucket
   * size of the hash map (i.e., `bucket_size`). It must support the tile-wide intrinsics
   * `ballot`, `shfl`
   * @param pair A key-value pair to insert into the hash map. The pair must be the same
   * for all threads in the  cooperative group tile
   * @param tile  The cooperative group tile
   * @return A boolean indicating success (true) or failure (false) of the insertion
   * operation - replacements are considered true.
   */

  __device__ bool upsert_replace(value_type const& pair, tile_type const& tile);

  /**
   * @brief Device-side cooperative find API that finds a single pair into the hash
   * map.
   * @tparam tile_type A cooperative group tile with a size that must match the bucket
   * size of the hash map (i.e., `bucket_size`). It must support the tile-wide intrinsics
   * `ballot`, `shfl`
   * @param key A key to find in the hash map. The key must be the same
   * for all threads in the  cooperative group tile
   * @param tile The cooperative group tile
   * @return The value of the key if it exists in the map or the `sentinel_value` if the
   * key does not exist in the hash map
   */

  __device__ mapped_type find(key_type const& key, tile_type const& tile);



  __device__ bool remove_exact(
    tile_type const& tile,
    value_type const& pair_to_remove);


  __device__ bool remove(
    tile_type const& tile,
    key_type const& key);

  __device__ value_type find_random(
    tile_type const& tile,
    Key const& key
    );

  __device__ value_type find_smaller_hash(
    tile_type const& tile,
    Key const& key);

  __device__ value_type pack_together(
    tile_type const& tile,
    key_type insert_key,
    mapped_type insert_val);



  __device__ bool insert_exact(
    tile_type const& tile,
    key_type insert_key,
    mapped_type insert_val);

  __device__ bool replace_exact(
    tile_type const& tile,
    key_type const& insert_key,
    mapped_type const& insert_val,
    value_type const& pair_to_remove);


  __device__ bool upsert_exact(
    tile_type const& tile,
    key_type insert_key,
    mapped_type insert_val,
    key_type old_key,
    mapped_type old_val
    );


  __device__ void stall_lock(tile_type const& tile, uint64_t bucket);

  __device__ void unlock(tile_type const& tile, uint64_t bucket);

  __device__ void lock_buckets(tile_type const& tile, uint64_t bucket0, uint64_t bucket1);

  __device__ void unlock_buckets(tile_type const& tile, uint64_t bucket0, uint64_t bucket1);

  /**
   * @brief Host-side API to randomize the hash functions used for the probing scheme.
   * This can be used when the hash table construction fails. The hash table must be
   * cleared after a call to this function.
   * @tparam RNG A pseudo-random number generator
   * @param rng An instantiation of the pseudo-random number generator
   */
  template <typename RNG>
  void randomize_hash_functions(RNG& rng);

  /**
   * @brief Compute the number of elements in the map
   * @return The number of elements in the map
   */
  size_type size(cudaStream_t stream = 0);

 private:
  template <typename InputIt, typename HashMap>
  friend __global__ void detail::kernels::tiled_insert_kernel(InputIt, InputIt, HashMap);

  template <typename InputIt, typename OutputIt, typename HashMap>
  friend __global__ void detail::kernels::tiled_find_kernel(InputIt,
                                                            InputIt,
                                                            OutputIt,
                                                            HashMap);

  template <int BlockSize, typename InputT, typename HashMap>
  friend __global__ void detail::kernels::count_kernel(const InputT,
                                                       std::size_t*,
                                                       HashMap);

  std::size_t capacity_;
  key_type sentinel_key_{};
  key_type tombstone_key_{};
  mapped_type sentinel_value_{};
  allocator_type allocator_;
  atomic_pair_allocator_type atomic_pairs_allocator_;
  pool_allocator_type pool_allocator_;
  size_type_allocator_type size_type_allocator_;

  atomic_pair_type* d_table_{};
  uint64_t * locks;
  std::shared_ptr<atomic_pair_type> table_;

  bool* d_build_success_;
  std::shared_ptr<bool> build_success_;

  Hash hf0_;
  Hash hf1_;

  std::size_t num_buckets_;
};

template <typename Key, typename T>
using p2bht8 = typename bght::p2bht<Key,
                                    T,
                                    bght::MurmurHash3_32<Key>,
                                    bght::equal_to<Key>,
                                    cuda::thread_scope_device,
                                    bght::cuda_allocator<char>,
                                    8>;

template <typename Key, typename T>
using p2bht16 = typename bght::p2bht<Key,
                                     T,
                                     bght::MurmurHash3_32<Key>,
                                     bght::equal_to<Key>,
                                     cuda::thread_scope_device,
                                     bght::cuda_allocator<char>,
                                     16>;

template <typename Key, typename T>
using p2bht32 = typename bght::p2bht<Key,
                                     T,
                                     bght::MurmurHash3_32<Key>,
                                     bght::equal_to<Key>,
                                     cuda::thread_scope_device,
                                     bght::cuda_allocator<char>,
                                     32>;

template <typename Key, typename T, uint bucket_size>
using p2bht_generic = typename bght::p2bht<Key,
                                    T,
                                    bght::MurmurHash3_32<Key>,
                                    bght::equal_to<Key>,
                                    cuda::thread_scope_device,
                                    bght::cuda_allocator<char>,
                                    bucket_size>;

}  // namespace bght

#include <bght/detail/p2bht_impl.cuh>
