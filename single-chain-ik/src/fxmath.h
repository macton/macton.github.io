/* fxmath.h — the only non-trivial integer math the demo needs that isn't a
 * single operator: a 64-bit floor square root.
 *
 * The length solver never calls this: it compares SQUARED lengths, so it stays
 * in integers with no root at all. isqrt64 is used only at the boundaries where
 * a real length is unavoidable — normalising the one aim rotation to a Q14 unit
 * vector, and reporting a leg's reach to the page. Inputs there are products of
 * subcell vectors, which overflow 32 bits, so the argument is u64. */
#ifndef SCIK_FXMATH_H
#define SCIK_FXMATH_H

#include <stdint.h>

static inline uint32_t isqrt64(uint64_t v) {
  uint64_t rem = v, root = 0, bit = 1ull << 62;
  while (bit > rem) bit >>= 2;
  while (bit) {
    if (rem >= root + bit) { rem -= root + bit; root = (root >> 1) + bit; }
    else                   { root >>= 1; }
    bit >>= 2;
  }
  return (uint32_t)root;
}

#endif
