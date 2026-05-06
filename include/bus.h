#ifndef BUS_H
#define BUS_H

#include <stdint.h>

typedef struct Bus Bus;

typedef struct Cart Cart;
typedef struct Timer Timer;
typedef struct Serial Serial;
typedef struct Ppu Ppu;
typedef struct Joypad Joypad;
typedef struct Dma Dma;
typedef struct Apu Apu;

/**
 * @brief Hardware interrupt source identifiers.
 *
 * The integer values match the bit positions in IF (0xFF0F) and IE
 * (0xFFFF), so they can be used directly as shift amounts.
 */
typedef enum {
  INT_VBLANK = 0,
  INT_STAT   = 1,
  INT_TIMER  = 2,
  INT_SERIAL = 3,
  INT_JOYPAD = 4,
} Interrupt;

Bus* bus_create(Cart* cart);  //NULL on fail
void bus_destroy(Bus* b);

uint8_t bus_read(Bus* b, uint16_t addr);  //Returns 0xFF on unmapped
void bus_write(Bus* b, uint16_t addr, uint8_t value); //Fails silently on unmapped

//No-tick funcs for internal use
uint8_t bus_peek(const Bus* b, uint16_t addr);
void bus_poke(Bus* b, uint16_t addr, uint8_t value);

void sys_tick(Bus* b, int mcycles);

/**
 * @brief Request a hardware interrupt.
 *
 * Sets the corresponding bit in the IF register (0xFF0F). Whether
 * the CPU actually services the interrupt depends on IME and the
 * matching bit in IE. Peripherals call this on the events they
 * generate (TIMA overflow, VBlank entry, etc.).
 */
void bus_request_interrupt(Bus* b, Interrupt which);

/**
 * @brief Get the timer attached to this bus.
 *
 * Used by tests (and later by the emu module's lifecycle) to reach
 * the timer for direct manipulation. Production CPU code never calls
 * this; the timer is reached through memory-mapped reads/writes.
 */
Timer* bus_timer(Bus* b);
Ppu*   bus_ppu(Bus* b);
Joypad* bus_joypad(Bus* b);
Dma*   bus_dma(Bus* b);
Apu*   bus_apu(Bus* b);

/**
 * @brief Total T-cycles that have elapsed since bus creation.
 *
 * Monotonically increasing; used by the emu loop to measure frame
 * boundaries.
 */
uint64_t bus_total_t_cycles(const Bus* b);

#endif