// wh_lock_shim.h
//
// Drop-in replacement for wormhole's `spinlock` and `rwlock` types/functions
// (declared upstream in lib.h around lines 304-366). When `WH_LOCK_SHIM` is
// defined, lib.h pulls this in *instead of* the upstream definitions, and
// wh_lock_shim.cpp provides the symbol bodies dispatching to a C++ lock
// chosen via `WH_LOCK_<NAME>`.
//
// The struct sizes are deliberately larger than upstream's 4-byte opaque
// to fit any of our primitives (ticket_lock has two cache-line-aligned
// counters = 128 bytes). Wormhole embeds these by value, so leaf size
// grows accordingly. That's intentional and unavoidable.

#ifndef WH_LOCK_SHIM_H
#define WH_LOCK_SHIM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 128 bytes covers ticket_lock (two alignas(64) atomics).
typedef struct { _Alignas(64) unsigned char storage[128]; } rwlock;
typedef struct { _Alignas(64) unsigned char storage[128]; } spinlock;

// spinlock — sortlock site
extern void  spinlock_init(spinlock *l);
extern void  spinlock_lock(spinlock *l);
extern bool  spinlock_trylock(spinlock *l);
extern void  spinlock_unlock(spinlock *l);

// rwlock — leaflock + metalock
extern void  rwlock_init(rwlock *l);

extern bool  rwlock_trylock_read(rwlock *l);
extern bool  rwlock_trylock_read_lp(rwlock *l);
extern bool  rwlock_trylock_read_nr(rwlock *l, uint16_t nr);
extern void  rwlock_lock_read(rwlock *l);
extern void  rwlock_unlock_read(rwlock *l);

extern bool  rwlock_trylock_write(rwlock *l);
extern bool  rwlock_trylock_write_nr(rwlock *l, uint16_t nr);
extern void  rwlock_lock_write(rwlock *l);

extern bool  rwlock_trylock_write_hp(rwlock *l);
extern bool  rwlock_trylock_write_hp_nr(rwlock *l, uint16_t nr);
extern void  rwlock_lock_write_hp(rwlock *l);

extern void  rwlock_unlock_write(rwlock *l);
extern void  rwlock_write_to_read(rwlock *l);

#ifdef __cplusplus
}
#endif

#endif  // WH_LOCK_SHIM_H
