// wh_lock_shim.cpp
//
// Implements the rwlock_* / spinlock_* C symbols declared in wh_lock_shim.h
// by dispatching to whichever lockbench primitive `WH_LOCK_<NAME>` selects.

#include "wh_lock_shim.h"

#include <new>
#include <type_traits>

#include "primitives/rw_lock.hpp"
#include "primitives/tas_lock.hpp"
#include "primitives/ttas_lock.hpp"
#include "primitives/cas_lock.hpp"
#include "primitives/ticket_lock.hpp"
#include "primitives/occ.hpp"

#if   defined(WH_LOCK_RW)
  using LockT = rw_lock;
  static constexpr bool kIsRw = true;
#elif defined(WH_LOCK_TAS)
  using LockT = tas_lock;     static constexpr bool kIsRw = false;
#elif defined(WH_LOCK_TTAS)
  using LockT = ttas_lock;    static constexpr bool kIsRw = false;
#elif defined(WH_LOCK_CAS)
  using LockT = cas_lock;     static constexpr bool kIsRw = false;
#elif defined(WH_LOCK_TICKET)
  using LockT = ticket_lock;  static constexpr bool kIsRw = false;
#elif defined(WH_LOCK_OCC)
  using LockT = occ_lock;     static constexpr bool kIsRw = false;
#else
#  error "wh_lock_shim.cpp: define exactly one WH_LOCK_<NAME>"
#endif

static_assert(sizeof(LockT)  <= sizeof(rwlock::storage),  "grow rwlock storage");
static_assert(alignof(LockT) <= alignof(rwlock),          "raise rwlock alignment");
static_assert(sizeof(LockT)  <= sizeof(spinlock::storage),"grow spinlock storage");
static_assert(alignof(LockT) <= alignof(spinlock),        "raise spinlock alignment");

static inline LockT* as_rw(rwlock* l)   { return std::launder(reinterpret_cast<LockT*>(l->storage)); }
static inline LockT* as_sp(spinlock* l) { return std::launder(reinterpret_cast<LockT*>(l->storage)); }

// Per-primitive try-lock. Templated so `if constexpr` actually discards
// the non-matching branches at instantiation time (in non-template
// contexts, every branch must compile, which we don't want).
template <class L>
static inline bool try_excl(L* lk) {
    if constexpr (std::is_same_v<L, rw_lock>) {
        return lk->try_write_lock();
    } else if constexpr (std::is_same_v<L, ticket_lock>) {
        return lk->try_lock();
    } else if constexpr (std::is_same_v<L, tas_lock>) {
        return !lk->flag.test_and_set(std::memory_order_acquire);
    } else if constexpr (std::is_same_v<L, ttas_lock>) {
        bool expected = false;
        return lk->state.compare_exchange_strong(
            expected, true, std::memory_order_acquire, std::memory_order_relaxed);
    } else if constexpr (std::is_same_v<L, cas_lock>) {
        bool expected = false;
        return lk->state.compare_exchange_strong(
            expected, true, std::memory_order_acquire, std::memory_order_relaxed);
    } else if constexpr (std::is_same_v<L, occ_lock>) {
        std::uint64_t v = lk->version.load(std::memory_order_relaxed);
        if (v & 1) return false;
        return lk->version.compare_exchange_strong(
            v, v + 1, std::memory_order_acquire, std::memory_order_relaxed);
    } else {
        static_assert(sizeof(L) == 0, "unsupported LockT in try_excl");
        return false;
    }
}

template <class L>
static inline bool try_shared(L* lk) {
    if constexpr (std::is_same_v<L, rw_lock>) {
        return lk->try_read_lock();
    } else {
        return try_excl(lk);  // exclusive-only fallback
    }
}

template <class L>
static inline void do_read_lock(L* lk) {
    if constexpr (std::is_same_v<L, rw_lock>) lk->read_lock();
    else                                       lk->lock();
}
template <class L>
static inline void do_read_unlock(L* lk) {
    if constexpr (std::is_same_v<L, rw_lock>) lk->read_unlock();
    else                                       lk->unlock();
}
template <class L>
static inline void do_write_to_read(L* lk) {
    if constexpr (std::is_same_v<L, rw_lock>) {
        lk->state.store(1, std::memory_order_release);
    } else {
        // exclusive-only: still holding exclusively, no transition needed
        (void)lk;
    }
}

extern "C" {

// ---- spinlock (sortlock) ----
void spinlock_init(spinlock* l)    { ::new (l->storage) LockT(); }
void spinlock_lock(spinlock* l)    { as_sp(l)->lock(); }
bool spinlock_trylock(spinlock* l) { return try_excl(as_sp(l)); }
void spinlock_unlock(spinlock* l)  { as_sp(l)->unlock(); }

// ---- rwlock (leaflock + metalock) ----
void rwlock_init(rwlock* l) { ::new (l->storage) LockT(); }

void rwlock_lock_read(rwlock* l)   { do_read_lock(as_rw(l)); }
void rwlock_unlock_read(rwlock* l) { do_read_unlock(as_rw(l)); }
void rwlock_lock_write(rwlock* l)    { as_rw(l)->lock(); }
void rwlock_unlock_write(rwlock* l)  { as_rw(l)->unlock(); }
void rwlock_lock_write_hp(rwlock* l) { as_rw(l)->lock(); }

bool rwlock_trylock_read(rwlock* l)     { return try_shared(as_rw(l)); }
bool rwlock_trylock_read_lp(rwlock* l)  { return try_shared(as_rw(l)); }
bool rwlock_trylock_write(rwlock* l)    { return try_excl(as_rw(l)); }
bool rwlock_trylock_write_hp(rwlock* l) { return try_excl(as_rw(l)); }

bool rwlock_trylock_read_nr(rwlock* l, uint16_t nr) {
    for (uint16_t i = 0; i < nr; ++i) {
        if (try_shared(as_rw(l))) return true;
        cpu_relax();
    }
    return false;
}
bool rwlock_trylock_write_nr(rwlock* l, uint16_t nr) {
    for (uint16_t i = 0; i < nr; ++i) {
        if (try_excl(as_rw(l))) return true;
        cpu_relax();
    }
    return false;
}
bool rwlock_trylock_write_hp_nr(rwlock* l, uint16_t nr) {
    return rwlock_trylock_write_nr(l, nr);
}

// Downgrade exclusive to shared.
void rwlock_write_to_read(rwlock* l) { do_write_to_read(as_rw(l)); }

}  // extern "C"
