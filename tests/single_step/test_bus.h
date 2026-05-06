/**
 * @file test_bus.h
 * @brief Harness-only helpers exposed by test_bus.c.
 *
 * The standard Bus interface is in bus.h; this header adds the
 * test-only functions for resetting state, raw peek/poke, and
 * inspecting the cycle counter.
 */

#ifndef TEST_BUS_H
#define TEST_BUS_H

#include "../../include/bus.h"
#include <stdint.h>

void     test_bus_reset(Bus* b);
uint8_t  test_bus_peek_raw(const Bus* b, uint16_t addr);
void     test_bus_poke_raw(Bus* b, uint16_t addr, uint8_t value);
uint64_t test_bus_t_cycles(const Bus* b);

#endif