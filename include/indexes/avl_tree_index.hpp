#pragma once
// Adapter exposing libcds BronsonAVLTreeMap behind the same get/put/remove
// interface used by hash_index<Lock>. Bronson AVL is a fine-grained-locking
// AVL tree with optimistic version-validated reads — the per-node lock type
// is the trait we plug Lock into via cds::sync::injecting_monitor<Lock>.
//
// Lifecycle requirements (must be set up by the caller — see cds_avl_bench.cpp):
//   1. cds::Initialize()
//   2. construct cds::urcu::gc<cds::urcu::general_buffered<>> singleton
//   3. attach the main thread (cds::threading::Manager::attachThread())
// Worker threads are auto-attached on first call via a thread_local guard.

#include <cds/init.h>
#include <cds/urcu/general_buffered.h>
#include <cds/container/bronson_avltree_map_rcu.h>
#include <cds/sync/injecting_monitor.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

using cds_rcu_gpb = cds::urcu::gc<cds::urcu::general_buffered<>>;

namespace detail {
// One per worker thread; attaches on first use, detaches at thread exit.
struct cds_thread_guard {
    bool owned = false;
    cds_thread_guard() {
        if (!cds::threading::Manager::isThreadAttached()) {
            cds::threading::Manager::attachThread();
            owned = true;
        }
    }
    ~cds_thread_guard() {
        if (owned) cds::threading::Manager::detachThread();
    }
};
inline void ensure_attached() {
    static thread_local cds_thread_guard g;
    (void)g;
}
}  // namespace detail

template <class Lock = std::mutex>
class avl_tree_index {
public:
    using key_type   = std::uint64_t;
    using value_type = std::uint64_t;

private:
    struct traits : public cds::container::bronson_avltree::traits {
        typedef std::less<key_type> less;
        typedef cds::sync::injecting_monitor<Lock> sync_monitor;
    };
    using map_type = cds::container::BronsonAVLTreeMap<
        cds_rcu_gpb, key_type, value_type, traits>;

public:
    // num_buckets ignored — kept for adapter parity with hash_index.
    explicit avl_tree_index(std::size_t /*num_buckets*/ = 0) {}

    avl_tree_index(const avl_tree_index&)            = delete;
    avl_tree_index& operator=(const avl_tree_index&) = delete;

    std::optional<value_type> get(key_type key) noexcept {
        detail::ensure_attached();
        std::optional<value_type> out;
        map_.find(key, [&](key_type const&, value_type& v) { out = v; });
        return out;
    }

    bool put(key_type key, value_type val) noexcept {
        detail::ensure_attached();
        // Data-oriented Bronson AVL update() takes a Func that receives
        // (bool bNew, key, &value) — same callback for insert and update paths.
        auto r = map_.update(
            key,
            [val](bool, key_type const&, value_type& v) { v = val; },
            true);
        return r.second;  // true if a new node was inserted
    }

    bool remove(key_type key) noexcept {
        detail::ensure_attached();
        return map_.erase(key);
    }

private:
    map_type map_;
};
