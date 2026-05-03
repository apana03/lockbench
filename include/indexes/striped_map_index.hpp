#pragma once
// Adapter exposing libcds StripedMap behind the same get/put/remove interface
// used by hash_index<Lock>. The Lock template parameter is forwarded to
// cds::container::striped_set::striping<Lock>, so any lockbench primitive
// (tas_lock, ttas_lock, ticket_lock, cas_lock) can be plugged in.

#include <cds/container/striped_map/std_list.h>
#include <cds/container/striped_map.h>

#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <utility>

template <class Lock = std::mutex>
class striped_map_index {
public:
  using key_type   = std::uint64_t;
  using value_type = std::uint64_t;

private:
  using bucket_type = std::list< std::pair<const key_type, value_type> >;
  // Default resizing policy for std::list buckets is load_factor_resizing<4>:
  // resize fires when item_count / bucket_count >= 4. The bucket array doubles
  // and every item is rehashed under scoped_full_lock (all stripe locks held).
  using map_type = cds::container::StripedMap<
      bucket_type,
      cds::opt::hash< std::hash<key_type> >,
      cds::opt::less< std::less<key_type> >,
      cds::opt::mutex_policy<
          cds::container::striped_set::striping< Lock >
      >
  >;

public:
  explicit striped_map_index(std::size_t num_buckets = 1 << 16)
      : map_(num_buckets) {}

  striped_map_index(const striped_map_index&) = delete;
  striped_map_index& operator=(const striped_map_index&) = delete;

  std::optional<value_type> get(key_type key) noexcept {
    std::optional<value_type> out;
    map_.find(key, [&](typename map_type::value_type& v) { out = v.second; });
    return out;
  }

  bool put(key_type key, value_type val) noexcept {
    return map_.insert(key, val);
  }

  bool remove(key_type key) noexcept {
    return map_.erase(key);
  }

private:
  map_type map_;
};
