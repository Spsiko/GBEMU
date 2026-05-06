/**
 * @file timer.c
 * @brief Game Boy timer peripheral implementation.
 *
 * Implementation strategy:
 *
 * We maintain a single 16-bit internal counter that ticks every T-cycle.
 * DIV (0xFF04) is the upper byte of this counter; writing any value to
 * DIV resets the entire counter to 0.
 *
 * TIMA (0xFF05) is incremented on the FALLING EDGE of a particular
 * "and-gate" output: (selected counter bit) AND (TAC enable bit). The
 * selected counter bit depends on TAC[1:0]:
 *
 *   00 -> bit 9  (4096 Hz)
 *   01 -> bit 3  (262144 Hz)
 *   10 -> bit 5  (65536 Hz)
 *   11 -> bit 7  (16384 Hz)
 *
 * To detect falling edges, we cache the previous tick's AND output as
 * `prev_and`. On every tick we recompute current_and; if prev_and was
 * 1 and current_and is 0, TIMA increments. This pattern correctly
 * handles all three subtle causes of unexpected TIMA increments:
 *
 *  - normal counter roll-over (selected bit toggles 1 -> 0)
 *  - TAC writes that change the selected bit, the enable, or both
 *  - DIV writes that zero the counter (selected bit forced to 0)
 *
 * TIMA overflow has a 1-M-cycle (4-T-cycle) delay. When TIMA increments
 * from 0xFF to 0x00, we set `overflow_pending` and a 4-T-cycle counter.
 * During those cycles TIMA reads as 0x00. When the counter expires, TIMA
 * is reloaded from TMA and the timer interrupt is requested.
 *
 * Edge cases not yet implemented (deferred to a Mooneye-polish pass):
 *  - Writing TIMA during the overflow delay window can cancel the
 *    pending reload+interrupt.
 *  - Writing TMA during the cycle that the reload fires uses the new
 *    TMA value.
 * cpu_instrs.gb does not exercise these.
 */

#include "timer.h"
#include "bus.h"
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

// Bit position of the internal counter that drives TIMA, indexed by
// TAC[1:0]. Lower index = higher frequency.
static const uint8_t kClockSelectBit[4] = { 9, 3, 5, 7 };

struct Timer {
  Bus* bus;
  uint16_t internal_counter; // DIV is the upper byte
  uint8_t  tima;
  uint8_t  tma;
  uint8_t  tac;
  bool     prev_and;         // previous tick's (selected_bit AND enable)
  uint8_t  overflow_delay;   // T-cycles remaining before reload (0 = no overflow pending)
};

Timer* timer_create(Bus* bus) {
  assert(bus != NULL);
  Timer* t = calloc(1, sizeof(Timer));
  if (t == NULL) return NULL;
  t->bus = bus;
  return t;
}

void timer_destroy(Timer* t) {
  free(t);
}

void timer_reset(Timer* t) {
  t->internal_counter = 0;
  t->tima = 0;
  t->tma = 0;
  t->tac = 0;
  t->prev_and = false;
  t->overflow_delay = 0;
}

// Compute the AND-gate output: (selected counter bit) AND (TAC enable bit).
// This is what TIMA watches for a falling edge.
static bool compute_and(const Timer* t) {
  bool enable = (t->tac & 0x04) != 0;
  uint8_t bit_pos = kClockSelectBit[t->tac & 0x03];
  bool sel_bit = (t->internal_counter & (1u << bit_pos)) != 0;
  return enable && sel_bit;
}

// Trigger the TIMA increment, including overflow detection.
// Called when a falling edge of compute_and is detected.
static void tima_increment(Timer* t) {
  if (t->tima == 0xFF) {
    // Overflow. TIMA reads as 0x00 for 4 T-cycles, then is reloaded
    // from TMA and the timer interrupt is requested.
    t->tima = 0x00;
    t->overflow_delay = 4;
  } else {
    t->tima++;
  }
}

void timer_tick_1t(Timer* t) {
  // Advance the internal counter.
  t->internal_counter++;

  // Detect a falling edge of (selected bit AND enable).
  bool current_and = compute_and(t);
  if (t->prev_and && !current_and) {
    tima_increment(t);
  }
  t->prev_and = current_and;

  // Service the deferred overflow reload, if any.
  if (t->overflow_delay > 0) {
    t->overflow_delay--;
    if (t->overflow_delay == 0) {
      t->tima = t->tma;
      bus_request_interrupt(t->bus, INT_TIMER);
    }
  }
}

uint8_t timer_read(const Timer* t, uint16_t addr) {
  switch (addr) {
    case 0xFF04: // DIV: upper byte of internal counter
      return (uint8_t)(t->internal_counter >> 8);
    case 0xFF05: // TIMA
      return t->tima;
    case 0xFF06: // TMA
      return t->tma;
    case 0xFF07: // TAC: upper 5 bits read as 1 (unimplemented)
      return (uint8_t)(t->tac | 0xF8);
    default:
      return 0xFF;
  }
}

void timer_write(Timer* t, uint16_t addr, uint8_t value) {
  switch (addr) {
    case 0xFF04: {
      // Any write to DIV resets the counter to 0. This may cause a
      // falling edge of the AND output (if the selected bit was 1
      // and enable was set), which would in turn increment TIMA.
      t->internal_counter = 0;
      bool current_and = compute_and(t); // false now; selected bit is 0
      if (t->prev_and && !current_and) {
        tima_increment(t);
      }
      t->prev_and = current_and;
      break;
    }
    case 0xFF05:
      // Writing TIMA. We do NOT yet implement the cancellation of a
      // pending overflow reload here -- see file header comment.
      t->tima = value;
      break;
    case 0xFF06:
      t->tma = value;
      break;
    case 0xFF07: {
      // Writing TAC may change the selected bit and/or the enable,
      // either of which can cause a falling edge of the AND output.
      t->tac = (uint8_t)(value & 0x07);  // only low 3 bits are meaningful
      bool current_and = compute_and(t);
      if (t->prev_and && !current_and) {
        tima_increment(t);
      }
      t->prev_and = current_and;
      break;
    }
    default:
      // Out-of-range writes silently ignored.
      break;
  }
}