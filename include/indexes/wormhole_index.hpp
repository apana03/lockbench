#pragma once
// Adapter exposing Wu et al.'s Wormhole behind the same get/put/remove
// interface used by hash_index<Lock> / striped_map_index / avl_tree_index.
//
// Wormhole's actual lock primitive is fixed at compile time at the static-lib
// level (see CMakeLists.txt's WH_LOCKS foreach + wh_lock_shim.cpp). This
// adapter does not template over Lock — there's one binary per lock variant.
//
// Thread lifecycle: each worker thread gets its own `struct wormref*` (lazy
// thread_local guard, mirrors the cds_thread_guard pattern in
// avl_tree_index.hpp). Wormhole's `wormref` participates in QSBR
// reclamation; without a per-thread ref, the QSBR engine deadlocks on
// removed-leaf reclamation.

extern "C" {
#include "lib.h"
#include "kv.h"
#include "wh.h"
}

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>

namespace wh_detail {

inline struct wormhole*& global_map_slot() {
    static struct wormhole* m = nullptr;
    return m;
}

// Custom kvmap_mm with a no-op free.
//
// Required for the WH_OCC_OPTIMISTIC variant (a reader walking a leaf
// without the leaflock can't safely dereference a kv that was just
// freed by a concurrent updater).
//
// Also enabled for the locked variants when WH_FAIR_MM is defined,
// so the per-op cost of free() is the same across all variants —
// otherwise wh-occ-opt's win on write-heavy workloads is partly
// "no free()" rather than purely "lock-free reader."
inline struct kv* occopt_mm_in_dup(struct kv* const kv, void*) {
    return kv_dup(kv);
}
inline struct kv* occopt_mm_out_dup(struct kv* const kv, struct kv* const out) {
    return kv_dup2(kv, out);
}
inline void occopt_mm_free_noop(struct kv* const, void*) {
    // intentional no-op: kvs leak for the lifetime of the process.
    // Bounded by run length × insert rate; tens to hundreds of MB
    // for a 3-second bench. NOT suitable for production / long runs.
}
inline const struct kvmap_mm occopt_mm = {
    occopt_mm_in_dup,
    occopt_mm_out_dup,
    occopt_mm_free_noop,
    nullptr,
};

struct wh_thread_guard {
    struct wormhole* attached_map = nullptr;
    struct wormref*  ref          = nullptr;

    wh_thread_guard() = default;

    // Re-attach if the current global map differs from the one we're
    // attached to. Multiple wormhole_index instances created sequentially
    // (e.g. in --mode both: single + race phases each construct one) need
    // the guard to follow the current map; otherwise we'd dereference a
    // wormref pointing at a destroyed map.
    void ensure_current() {
        struct wormhole* cur = global_map_slot();
        if (cur == attached_map) return;
        if (ref && attached_map) wormhole_unref(ref);
        attached_map = cur;
        ref = cur ? wormhole_ref(cur) : nullptr;
    }

    ~wh_thread_guard() {
        // Only unref while the original map is still alive. The main
        // thread's TLS dtor runs after the last wormhole_index's dtor
        // (which nulls global_map_slot + calls wormhole_destroy).
        if (ref && global_map_slot() == attached_map && attached_map != nullptr)
            wormhole_unref(ref);
    }
    wh_thread_guard(const wh_thread_guard&)            = delete;
    wh_thread_guard& operator=(const wh_thread_guard&) = delete;
};

// Expose the per-thread guard so wormhole_index's destructor can detach
// the calling thread (typically main) before wormhole_destroy is called.
inline wh_thread_guard& tls_guard() {
    static thread_local wh_thread_guard g;
    return g;
}

inline struct wormref* thread_ref() {
    auto& g = tls_guard();
    g.ensure_current();
    return g.ref;
}

// Big-endian 64-bit encoding so wormhole's lex order matches numeric.
inline std::array<std::uint8_t, 8> encode_be(std::uint64_t k) {
    std::array<std::uint8_t, 8> out{};
    for (int i = 7; i >= 0; --i) { out[i] = static_cast<std::uint8_t>(k); k >>= 8; }
    return out;
}

}  // namespace wh_detail

class wormhole_index {
public:
    using key_type   = std::uint64_t;
    using value_type = std::uint64_t;

    explicit wormhole_index(std::size_t /*unused*/ = 0) {
#if defined(WH_OCC_OPTIMISTIC) || defined(WH_FAIR_MM)
        // OCC-optimistic: required for safety (no UAF on freed kvs).
        // WH_FAIR_MM: optionally enabled for locked variants too so per-op
        // free() cost is uniform across the comparison. Without this, the
        // locked variants pay a per-op free() cost that wh-occ-opt avoids,
        // which inflates the apparent reader-strategy speedup.
        map_ = wormhole_create(&wh_detail::occopt_mm);
#else
        map_ = wormhole_create(&kvmap_mm_dup);
#endif
        wh_detail::global_map_slot() = map_;
    }

    ~wormhole_index() {
        // Detach the calling thread (main, typically) from this map before
        // destroying it. Worker threads detach in their own TLS dtor when
        // they exit, which happens before this dtor runs (workers are
        // joined inside race_test).
        auto& g = wh_detail::tls_guard();
        if (g.ref && g.attached_map == map_) {
            wormhole_unref(g.ref);
            g.ref = nullptr;
            g.attached_map = nullptr;
        }
        wh_detail::global_map_slot() = nullptr;
        if (map_) wormhole_destroy(map_);
    }

    wormhole_index(const wormhole_index&)            = delete;
    wormhole_index& operator=(const wormhole_index&) = delete;

    std::optional<value_type> get(key_type k) noexcept {
        auto* r = wh_detail::thread_ref();
        if (!r) return std::nullopt;
        wormhole_resume(r);

        auto kbuf = wh_detail::encode_be(k);
        struct kref kr;
        kref_ref_hash32(&kr, kbuf.data(), 8);
        alignas(16) unsigned char buf[sizeof(struct kv) + 8 + 8];
        struct kv* out = reinterpret_cast<struct kv*>(buf);
        out->klen = 0; out->vlen = 0;

        struct kv* res = wormhole_get(r, &kr, out);
        std::optional<value_type> ret;
        if (res && res->vlen == 8) {
            value_type v;
            std::memcpy(&v, res->kv + res->klen, 8);
            ret = v;
        }
        wormhole_park(r);
        return ret;
    }

    bool put(key_type k, value_type v) noexcept {
        auto* r = wh_detail::thread_ref();
        if (!r) return false;
        wormhole_resume(r);

        auto kbuf = wh_detail::encode_be(k);
        struct kv* mykv = kv_create(kbuf.data(), 8, &v, 8);
        bool ok = mykv && wormhole_put(r, mykv);
        if (mykv) std::free(mykv);  // safe: kvmap_mm_dup copied internally

        wormhole_park(r);
        return ok;
    }

    bool remove(key_type k) noexcept {
        auto* r = wh_detail::thread_ref();
        if (!r) return false;
        wormhole_resume(r);

        auto kbuf = wh_detail::encode_be(k);
        struct kref kr;
        kref_ref_hash32(&kr, kbuf.data(), 8);
        bool ok = wormhole_del(r, &kr);

        wormhole_park(r);
        return ok;
    }

private:
    struct wormhole* map_ = nullptr;
};
