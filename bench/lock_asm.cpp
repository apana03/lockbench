// compile with: clang++ -std=c++20 -O3 -march=native -S -o lock_asm.s bench/lock_asm.cpp -Iinclude
// this file exists just to see the assembly for each lock's lock/unlock

#include "primitives/util.hpp"
#include "primitives/tas_lock.hpp"
#include "primitives/ttas_lock.hpp"
#include "primitives/cas_lock.hpp"
#include "primitives/ticket_lock.hpp"
#include "primitives/rw_lock.hpp"
#include "primitives/occ.hpp"

// noinline so the compiler doesn't fold them away
#define NOINLINE __attribute__((noinline))

NOINLINE void tas_lock_fn(tas_lock& l)     { l.lock(); }
NOINLINE void tas_unlock_fn(tas_lock& l)   { l.unlock(); }

NOINLINE void ttas_lock_fn(ttas_lock& l)   { l.lock(); }
NOINLINE void ttas_unlock_fn(ttas_lock& l) { l.unlock(); }

NOINLINE void cas_lock_fn(cas_lock& l)     { l.lock(); }
NOINLINE void cas_unlock_fn(cas_lock& l)   { l.unlock(); }

NOINLINE void ticket_lock_fn(ticket_lock& l)   { l.lock(); }
NOINLINE void ticket_unlock_fn(ticket_lock& l) { l.unlock(); }

NOINLINE void rw_read_lock_fn(rw_lock& l)    { l.read_lock(); }
NOINLINE void rw_read_unlock_fn(rw_lock& l)  { l.read_unlock(); }
NOINLINE void rw_write_lock_fn(rw_lock& l)   { l.write_lock(); }
NOINLINE void rw_write_unlock_fn(rw_lock& l) { l.write_unlock(); }

NOINLINE void occ_write_lock_fn(occ_lock& l)   { l.write_lock(); }
NOINLINE void occ_write_unlock_fn(occ_lock& l) { l.write_unlock(); }
NOINLINE uint64_t occ_read_begin_fn(const occ_lock& l)  { return l.read_begin(); }
NOINLINE bool occ_read_validate_fn(const occ_lock& l, uint64_t v) { return l.read_validate(v); }
