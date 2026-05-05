/**
 * @file cpu.h
 * @brief Sharp LR35902 (SM83) CPU emulator.
 *
 * The CPU fetches and executes opcodes through a Bus. It holds no
 * direct references to peripherals; all side effects flow through
 * bus_read, bus_write, and sys_tick (cycle-accurate, M-cycle granularity).
 *
 * Construction requires a Bus. cpu_reset sets registers to the state
 * expected after the DMG bootrom would have run (bootrom is not
 * implemented; see decisions.md).
 */

#ifndef CPU_H
#define CPU_H

#include <stdint.h>

typedef struct CPU CPU;
typedef struct Bus Bus;

/**
 * @brief Create a CPU attached to the given bus.
 *
 * @param bus The bus the CPU will use for all memory access. Must not be NULL.
 * @return Pointer to the new CPU, or NULL on allocation failure.
 */
CPU* cpu_create(Bus* bus);

/**
 * @brief Destroy a CPU and free its memory.
 *
 * @param cpu The CPU to destroy. Safe to call with NULL.
 */
void cpu_destroy(CPU* cpu);

/**
 * @brief Set the CPU to its post-bootrom initial state.
 *
 * PC is set to 0x0100, registers to their documented post-bootrom
 * values, IME is disabled, halted flag cleared.
 *
 * @param cpu The CPU to reset. Must not be NULL.
 */
void cpu_reset(CPU* cpu);

/**
 * @brief Execute one instruction.
 *
 * Handles pending interrupts and the halted state before dispatch.
 * Cycles are spent through sys_tick as the instruction executes.
 *
 * @param cpu The CPU to step. Must not be NULL.
 */
void cpu_step(CPU* cpu);

#endif