/**
 * @file test_bus.c
 * @brief Test-only Bus implementation backed by flat 64KB RAM.
 *
 * Provides the same external interface as the real bus.c (bus_create,
 * bus_destroy, bus_read, bus_write, bus_peek, bus_poke, sys_tick) but
 * with no cart, no peripherals, no region dispatch. Every address in
 * 0x0000-0xFFFF maps to a single byte in a flat array.
 *
 * Linked into the SingleStepTests harness instead of the real bus.o.
 * The CPU is unchanged; it sees a Bus pointer and calls the same
 * functions. Substitution is at link time.
 *
 * Extras for the harness:
 *   - test_bus_poke_raw / test_bus_peek_raw: read/write without ticking,
 *     used to set initial RAM state from JSON and verify final state.
 */

#include "test_bus.h"
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

struct Bus {
  uint8_t  ram[0x10000];
  uint64_t t_cycles;
};

// Cart parameter is ignored; tests use no cart.
Bus* bus_create(Cart* cart) {
  (void)cart;
  Bus* b = calloc(1, sizeof(Bus));
  return b;
}

void bus_destroy(Bus* b) {
  free(b);
}

uint8_t bus_read(Bus* b, uint16_t addr) {
  sys_tick(b, 1);
  return b->ram[addr];
}

void bus_write(Bus* b, uint16_t addr, uint8_t value) {
  sys_tick(b, 1);
  b->ram[addr] = value;
}

uint8_t bus_peek(const Bus* b, uint16_t addr) {
  return b->ram[addr];
}

void bus_poke(Bus* b, uint16_t addr, uint8_t value) {
  b->ram[addr] = value;
}

void sys_tick(Bus* b, int mcycles) {
  b->t_cycles += (uint64_t)mcycles * 4;
}

// --- Harness helpers (not part of the production Bus interface) ---

/** @brief Reset all RAM to zero and clear the cycle counter. */
void test_bus_reset(Bus* b) {
  assert(b != NULL);
  for (int i = 0; i < 0x10000; i++) b->ram[i] = 0;
  b->t_cycles = 0;
}

/** @brief Read RAM without ticking (for verification). */
uint8_t test_bus_peek_raw(const Bus* b, uint16_t addr) {
  return b->ram[addr];
}

/** @brief Write RAM without ticking (for setup). */
void test_bus_poke_raw(Bus* b, uint16_t addr, uint8_t value) {
  b->ram[addr] = value;
}

/** @brief Current T-cycle count. */
uint64_t test_bus_t_cycles(const Bus* b) {
  return b->t_cycles;
}