/**
 * @file cpu_internal.h
 * @brief Private CPU internals shared between cpu.c and cpu_ops.c.
 *
 * This header exposes the CPU struct definition, register layout,
 * flag constants, and internal helpers needed by opcode implementations.
 *
 * @warning Do not include this header from outside the cpu module.
 * External code should include cpu.h only.
 */

#ifndef CPU_INTERNAL_H
#define CPU_INTERNAL_H

#include "cpu.h"
#include "bus.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Interrupt master enable state machine.
 *
 * EI has a one-instruction delay before interrupts are actually enabled.
 * IME_ENABLING is the transient state set by EI; it becomes IME_ENABLED
 * after the instruction following EI completes.
 */
typedef enum {
  IME_DISABLED,
  IME_ENABLING,
  IME_ENABLED,
} ImeState;

/**
 * @brief CPU register file.
 *
 * Stored as individual 8-bit registers. Use the reg_get_xx / reg_set_xx
 * helpers below for 16-bit pair access. The low nibble of F is always
 * zero on real hardware; reg_get_af and reg_set_af enforce this.
 *
 *   F register bit layout:
 *   7: Zero (Z)     5: Half-carry (H)
 *   6: Subtract (N) 4: Carry (C)
 *   3-0: Always zero
 */
typedef struct {
  uint8_t a, f;
  uint8_t b, c;
  uint8_t d, e;
  uint8_t h, l;
} Registers;

/**
 * @brief Flag bit masks for register F.
 */
enum CpuFlag {
  FLAG_Z = 0x80,  // Zero
  FLAG_N = 0x40,  // Subtract
  FLAG_H = 0x20,  // Half-carry
  FLAG_C = 0x10,  // Carry
};

//  F Register Atlus/Guide
//    ┌-> Carry
//  ┌-+> Subtraction
//  | |
// 1111 0000
// | |
// └-+> Zero
//   └-> Half Carry

/**
 * @brief Full CPU state. Opaque to external code (see cpu.h).
 */
struct CPU {
  Bus*      bus;
  Registers r;
  uint16_t  pc;
  uint16_t  sp;
  ImeState  ime;
  bool      halted;
  bool      halt_bug_pending;  // Set by op_halt when the HALT bug
                               // fires; consumed by the next opcode
                               // fetch in cpu_execute_instruction
                               // to suppress PC increment.
};

/**
 * @brief Opcode function type.
 *
 * Each opcode receives the CPU and the raw opcode byte. The byte is
 * passed so that decode-based dispatchers (LD r,r, ALU families, CB
 * bit ops, CB rotates/shifts) can extract register indices from it.
 * Opcodes that don't need the byte should cast it to (void).
 */
typedef void (*OpFn)(CPU* c, uint8_t opcode);

// --- Register pair helpers ---

/** @brief Read the 16-bit BC register pair. */
static inline uint16_t reg_get_bc(const Registers* r) {
  return ((uint16_t)r->b << 8) | r->c;
}

/** @brief Read the 16-bit DE register pair. */
static inline uint16_t reg_get_de(const Registers* r) {
  return ((uint16_t)r->d << 8) | r->e;
}

/** @brief Read the 16-bit HL register pair. */
static inline uint16_t reg_get_hl(const Registers* r) {
  return ((uint16_t)r->h << 8) | r->l;
}

/** @brief Read the 16-bit AF register pair. F's low nibble is masked to 0. */
static inline uint16_t reg_get_af(const Registers* r) {
  return ((uint16_t)r->a << 8) | (uint16_t)(r->f & 0xF0);
}

/** @brief Write the 16-bit BC register pair. */
static inline void reg_set_bc(Registers* r, uint16_t v) {
  r->b = (uint8_t)(v >> 8);
  r->c = (uint8_t)(v & 0xFF);
}

/** @brief Write the 16-bit DE register pair. */
static inline void reg_set_de(Registers* r, uint16_t v) {
  r->d = (uint8_t)(v >> 8);
  r->e = (uint8_t)(v & 0xFF);
}

/** @brief Write the 16-bit HL register pair. */
static inline void reg_set_hl(Registers* r, uint16_t v) {
  r->h = (uint8_t)(v >> 8);
  r->l = (uint8_t)(v & 0xFF);
}

/** @brief Write the 16-bit AF register pair. F's low nibble is forced to 0. */
static inline void reg_set_af(Registers* r, uint16_t v) {
  r->a = (uint8_t)(v >> 8);
  r->f = (uint8_t)(v & 0xF0);
}

// --- Internal step entry point (test harness only) ---

/**
 * @brief Execute a single instruction without checking for pending
 *        interrupts and without honoring HALT.
 *
 * This is cpu_step minus the interrupt-service prelude. It exists
 * for the SingleStepTests harness, which tests CPU instructions in
 * isolation and must not see an interrupt fire in place of the
 * opcode under test. Production code should use cpu_step.
 */
void cpu_execute_instruction(CPU* c);

// --- Fetch helpers (used by opcodes in cpu_ops.c) ---

/**
 * @brief Fetch an 8-bit value from PC and advance PC by 1.
 *
 * Spends 1 M-cycle via sys_tick (through bus_read).
 */
static inline uint8_t fetch8(CPU* c) {
  return bus_read(c->bus, c->pc++);
}

/**
 * @brief Fetch a 16-bit little-endian value from PC and advance PC by 2.
 *
 * Spends 2 M-cycles total (one per byte). Low byte is at PC, high at PC+1.
 */
static inline uint16_t fetch16(CPU* c) {
  uint8_t lo = fetch8(c);
  uint8_t hi = fetch8(c);
  return ((uint16_t)hi << 8) | lo;
}

// --- Opcode prototypes (defined in cpu_ops.c, referenced by tables in cpu.c) ---

void op_nop(CPU* c, uint8_t opcode);

void op_ld_b_n(CPU* c, uint8_t opcode);
void op_ld_c_n(CPU* c, uint8_t opcode);
void op_ld_d_n(CPU* c, uint8_t opcode);
void op_ld_e_n(CPU* c, uint8_t opcode);
void op_ld_h_n(CPU* c, uint8_t opcode);
void op_ld_l_n(CPU* c, uint8_t opcode);
void op_ld_a_n(CPU* c, uint8_t opcode);

void op_ld_bc_nn(CPU* c, uint8_t opcode);
void op_ld_de_nn(CPU* c, uint8_t opcode);
void op_ld_hl_nn(CPU* c, uint8_t opcode);
void op_ld_sp_nn(CPU* c, uint8_t opcode);

void op_ld_r_r(CPU* c, uint8_t opcode);

void op_alu_a_r(CPU* c, uint8_t opcode);
void op_alu_a_n(CPU* c, uint8_t opcode);

void op_inc_dec_r(CPU* c, uint8_t opcode);
void op_inc_dec_rr(CPU* c, uint8_t opcode);

void op_add_hl_rr(CPU* c, uint8_t opcode);

void op_jr_e(CPU* c, uint8_t opcode);
void op_jr_cc_e(CPU* c, uint8_t opcode);

void op_jp_nn(CPU* c, uint8_t opcode);
void op_jp_cc_nn(CPU* c, uint8_t opcode);
void op_jp_hl(CPU* c, uint8_t opcode);

void op_call_nn(CPU* c, uint8_t opcode);
void op_call_cc_nn(CPU* c, uint8_t opcode);

void op_ret(CPU* c, uint8_t opcode);
void op_ret_cc(CPU* c, uint8_t opcode);
void op_reti(CPU* c, uint8_t opcode);

void op_push_rr(CPU* c, uint8_t opcode);
void op_pop_rr(CPU* c, uint8_t opcode);

void op_rst(CPU* c, uint8_t opcode);

void op_rotate_a(CPU* c, uint8_t opcode);

void op_daa(CPU* c, uint8_t opcode);
void op_cpl(CPU* c, uint8_t opcode);
void op_scf(CPU* c, uint8_t opcode);
void op_ccf(CPU* c, uint8_t opcode);

void op_di(CPU* c, uint8_t opcode);
void op_ei(CPU* c, uint8_t opcode);
void op_halt(CPU* c, uint8_t opcode);
void op_stop(CPU* c, uint8_t opcode);

// Indirect loads to/from A through register pairs.
void op_ld_mbc_a(CPU* c, uint8_t opcode);
void op_ld_mde_a(CPU* c, uint8_t opcode);
void op_ld_a_mbc(CPU* c, uint8_t opcode);
void op_ld_a_mde(CPU* c, uint8_t opcode);
void op_ld_mhli_a(CPU* c, uint8_t opcode);
void op_ld_mhld_a(CPU* c, uint8_t opcode);
void op_ld_a_mhli(CPU* c, uint8_t opcode);
void op_ld_a_mhld(CPU* c, uint8_t opcode);

// LDH (high-page 0xFF00) accesses.
void op_ldh_n_a(CPU* c, uint8_t opcode);
void op_ldh_a_n(CPU* c, uint8_t opcode);
void op_ldh_mc_a(CPU* c, uint8_t opcode);
void op_ldh_a_mc(CPU* c, uint8_t opcode);

// Absolute address loads.
void op_ld_mnn_a(CPU* c, uint8_t opcode);
void op_ld_a_mnn(CPU* c, uint8_t opcode);
void op_ld_mnn_sp(CPU* c, uint8_t opcode);

// SP/HL specials.
void op_ld_sp_hl(CPU* c, uint8_t opcode);
void op_ld_hl_sp_e(CPU* c, uint8_t opcode);
void op_add_sp_e(CPU* c, uint8_t opcode);

// CB-prefix rotates and shifts (CB 0x00 - CB 0x3F).
void op_cb_rotate_shift(CPU* c, uint8_t opcode);

// CB-prefix bit operations.
void op_cb_bit(CPU* c, uint8_t opcode);  // CB 0x40 - 0x7F
void op_cb_res(CPU* c, uint8_t opcode);  // CB 0x80 - 0xBF
void op_cb_set(CPU* c, uint8_t opcode);  // CB 0xC0 - 0xFF

void op_ld_mhl_n(CPU* c, uint8_t opcode); // 0x36

#endif