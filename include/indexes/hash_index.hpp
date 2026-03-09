#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>

// Concurrent hash table with per-bucket locking
// Each bucket has a few inline slots to avoid allocating for small buckets,
// and a linked list for overflow. The lock sits in the same cache line as the data.
// Templated on the lock type so we can swap them out for benchmarks.
template <class Lock, std::size_t InlineSlots = 4>
class hash_index {
public:
  using key_type   = std::uint64_t;
  using value_type = std::uint64_t;

  // num_buckets should be a power of two
  explicit hash_index(std::size_t num_buckets = 1 << 16)
      : mask_(num_buckets - 1), buckets_(new bucket[num_buckets]) {}

  ~hash_index() {
    const std::size_t n = mask_ + 1;
    for (std::size_t i = 0; i < n; ++i) {
      chain_node* c = buckets_[i].chain;
      while (c) { auto* next = c->next; delete c; c = next; }
    }
    delete[] buckets_;
  }

  hash_index(const hash_index&) = delete;
  hash_index& operator=(const hash_index&) = delete;

  std::optional<value_type> get(key_type key) noexcept {
    auto& b = bucket_for(key);
    b.lock.lock();
    auto result = find_in_bucket(b, key);
    b.lock.unlock();
    return result;
  }

  bool put(key_type key, value_type val) noexcept {
    auto& b = bucket_for(key);
    b.lock.lock();
    bool inserted = upsert_in_bucket(b, key, val);
    b.lock.unlock();
    return inserted;
  }

  bool remove(key_type key) noexcept {
    auto& b = bucket_for(key);
    b.lock.lock();
    bool removed = remove_from_bucket(b, key);
    b.lock.unlock();
    return removed;
  }

  // lookup using shared read lock (only works with rw_lock)
  template <class L = Lock>
  auto get_shared(key_type key) noexcept
      -> decltype(std::declval<L>().read_lock(), std::optional<value_type>{}) {
    auto& b = bucket_for(key);
    b.lock.read_lock();
    auto result = find_in_bucket(b, key);
    b.lock.read_unlock();
    return result;
  }

  // optimistic read for OCC - no lock, just check version after
  template <class L = Lock>
  auto get_optimistic(key_type key) noexcept
      -> decltype(std::declval<const L>().read_begin(), std::optional<value_type>{}) {
    auto& b = bucket_for(key);
    for (;;) {
      auto v = b.lock.read_begin();
      auto result = find_in_bucket(b, key);
      if (b.lock.read_validate(v)) return result;
      // someone wrote while we were reading, try again
    }
  }

private:
  struct chain_node {
    key_type   key;
    value_type val;
    chain_node* next = nullptr;
  };

  struct alignas(64) bucket {
    Lock lock{};
    std::uint8_t count = 0;
    std::array<key_type,   InlineSlots> keys{};
    std::array<value_type, InlineSlots> vals{};
    chain_node* chain = nullptr;
  };

  std::size_t mask_;
  bucket* buckets_;

  bucket& bucket_for(key_type key) noexcept {
    // fibonacci hashing
    std::size_t h = key * 11400714819323198485ULL;
    return buckets_[h & mask_];
  }

  std::optional<value_type> find_in_bucket(const bucket& b, key_type key) const noexcept {
    for (std::uint8_t i = 0; i < b.count; ++i) {
      if (b.keys[i] == key) return b.vals[i];
    }
    for (auto* c = b.chain; c; c = c->next) {
      if (c->key == key) return c->val;
    }
    return std::nullopt;
  }

  bool upsert_in_bucket(bucket& b, key_type key, value_type val) noexcept {
    // check if key already exists
    for (std::uint8_t i = 0; i < b.count; ++i) {
      if (b.keys[i] == key) { b.vals[i] = val; return false; }
    }
    for (auto* c = b.chain; c; c = c->next) {
      if (c->key == key) { c->val = val; return false; }
    }
    // new key - put in inline slot if there's room, otherwise chain
    if (b.count < InlineSlots) {
      b.keys[b.count] = key;
      b.vals[b.count] = val;
      ++b.count;
    } else {
      auto* node = new chain_node{key, val, b.chain};
      b.chain = node;
    }
    return true;
  }

  bool remove_from_bucket(bucket& b, key_type key) noexcept {
    for (std::uint8_t i = 0; i < b.count; ++i) {
      if (b.keys[i] == key) {
        // swap with last to avoid shifting
        --b.count;
        b.keys[i] = b.keys[b.count];
        b.vals[i] = b.vals[b.count];
        return true;
      }
    }
    chain_node** pp = &b.chain;
    while (*pp) {
      if ((*pp)->key == key) {
        auto* victim = *pp;
        *pp = victim->next;
        delete victim;
        return true;
      }
      pp = &(*pp)->next;
    }
    return false;
  }
};
