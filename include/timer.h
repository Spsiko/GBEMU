/**
 * @file timer.h
 * @brief Game Boy timer peripheral (DIV, TIMA, TMA, TAC).
 *
 * The timer is a clocked peripheral driven from sys_tick. It owns
 * the four registers at 0xFF04 - 0xFF07:
 *
 *   0xFF04 DIV   read: upper byte of the 16-bit internal counter
 *                write: any value resets the entire counter to 0
 *   0xFF05 TIMA  read/write: the user-visible timer counter
 *   0xFF06 TMA   read/write: the value TIMA reloads to on overflow
 *   0xFF07 TAC   read/write: bit 2 = enable, bits 1:0 = clock select
 *
 * On TIMA overflow (0xFF -> 0x00), the timer requests a timer
 * interrupt (IF bit 2) and reloads TIMA from TMA. The reload and
 * interrupt are delayed by 1 M-cycle (4 T-cycles) -- during the
 * delay, TIMA reads as 0x00.
 */
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

typedef struct Bus Bus;
typedef struct Timer Timer;

/**
 * @brief Create a timer attached to the given bus.
 *
 * The bus is used only to call bus_request_interrupt on TIMA
 * overflow; the timer does not otherwise hold bus state.
 *
 * @return New Timer, or NULL on allocation failure.
 */
Timer* timer_create(Bus* bus);

/** @brief Free the timer. */
void timer_destroy(Timer* t);

/**
 * @brief Reset the timer to post-bootrom state.
 *
 * Sets the internal counter, TIMA, TMA, and TAC to their documented
 * post-boot values (DIV upper byte = 0xAB on the standard DMG boot
 * sequence, but we approximate with counter = 0 for simplicity since
 * the bootrom is not implemented).
 */
void timer_reset(Timer* t);

/**
 * @brief Advance the timer by one T-cycle.
 *
 * Called from sys_tick. Updates the internal counter, detects the
 * falling-edge events that drive TIMA, and handles the deferred
 * overflow reload.
 */
void timer_tick_1t(Timer* t);

/**
 * @brief Read a timer register (0xFF04 - 0xFF07).
 *
 * Returns 0xFF for any other address.
 */
uint8_t timer_read(const Timer* t, uint16_t addr);

/**
 * @brief Write a timer register (0xFF04 - 0xFF07).
 *
 * Writes to other addresses are silently ignored.
 */
void timer_write(Timer* t, uint16_t addr, uint8_t value);

#endif