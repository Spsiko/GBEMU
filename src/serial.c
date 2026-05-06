#include "serial.h"
#include <stdio.h>
#include <stdlib.h>

struct Serial {
  uint8_t sb;
  uint8_t sc;
};

Serial* serial_create(void) {
  return calloc(1, sizeof(Serial));
}

void serial_destroy(Serial* s) {
  free(s);
}

void serial_reset(Serial* s) {
  s->sb = 0;
  s->sc = 0;
}

uint8_t serial_read(const Serial* s, uint16_t addr) {
  switch (addr) {
    case 0xFF01: return s->sb;
    // SC: upper bits 0x7E read as 1 (unimplemented bits). Bit 7
    // (transfer in progress) is held low because we complete
    // synchronously on write.
    case 0xFF02: return (uint8_t)(s->sc | 0x7E);
    default: return 0xFF;
  }
}

void serial_write(Serial* s, uint16_t addr, uint8_t value) {
  switch (addr) {
    case 0xFF01:
      s->sb = value;
      break;
    case 0xFF02:
      s->sc = value;
      // Transfer started: bit 7 set. Complete it synchronously.
      if (value & 0x80) {
        putchar(s->sb);
        fflush(stdout);
        s->sc = (uint8_t)(value & 0x7F); // clear transfer-in-progress bit
      }
      break;
    default:
      break;
  }
}