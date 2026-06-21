#include "tanks_turn.h"

/* Branchy on input bits, but n is tiny and this is not a hot path. */
void tanks_turn(uint16_t* ang, const uint8_t* in, uint32_t n, uint16_t step) {
  for (uint32_t i = 0; i < n; i++) {
    int32_t d = (int32_t)((in[i] & IN_RIGHT) != 0) - (int32_t)((in[i] & IN_LEFT) != 0);
    ang[i] = (uint16_t)((int32_t)ang[i] + (int32_t)step * d);   /* wraps mod 65536 */
  }
}
