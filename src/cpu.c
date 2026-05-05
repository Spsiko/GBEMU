/**
 * @file cpu.c
 * @brief Sharp LR35902 (SM83) CPU emulator: core implementation.
 *
 * Contains the CPU lifecycle (create, destroy, reset), the main step
 * function, interrupt servicing, and the opcode dispatch tables.
 * Opcode implementations live in cpu_ops.c.
 *
 * The CPU operates at M-cycle granularity. Every memory access flows
 * through bus_read / bus_write, which advance the system clock via
 * sys_tick before the access takes place. Opcodes that include
 * internal cycles (no memory access) call sys_tick directly.
 */

#include "cpu_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

// Opcode dispatch tables. Defined at the bottom of this file.
static const OpFn ops[256];
static const OpFn cb_ops[256];

static void service_interrupt(CPU* c);
static void dispatch(CPU* c, const OpFn* table, uint8_t opcode, bool cb);

// --- Lifecycle ---

CPU* cpu_create(Bus* bus) {
  assert(bus != NULL);

  CPU* cpu = calloc(1, sizeof(CPU));
  if (cpu == NULL) return NULL;

  cpu->bus = bus;
  cpu_reset(cpu);

  return cpu;
}

void cpu_destroy(CPU* cpu) {
  if (cpu == NULL) return;
  free(cpu);
}

void cpu_reset(CPU* cpu) {
  assert(cpu != NULL);

  cpu->pc = 0x0100;
  cpu->sp = 0xFFFE;

  cpu->r.a = 0x01;
  cpu->r.f = FLAG_Z | FLAG_H | FLAG_C;
  cpu->r.b = 0x00;
  cpu->r.c = 0x13;
  cpu->r.d = 0x00;
  cpu->r.e = 0xD8;
  cpu->r.h = 0x01;
  cpu->r.l = 0x4D;

  cpu->ime = IME_DISABLED;
  cpu->halted = false;
  cpu->halt_bug_pending = false;
}

// --- Step ---

void cpu_step(CPU* c) {
  // Service pending interrupt if IME enabled and one is pending.
  if (c->ime == IME_ENABLED) {
    uint8_t ie = bus_peek(c->bus, 0xFFFF);
    uint8_t if_reg = bus_peek(c->bus, 0xFF0F);
    if ((ie & if_reg & 0x1F) != 0) {
      service_interrupt(c);
      return;
    }
  }

  // If halted, wake on pending interrupt or sleep one M-cycle.
  if (c->halted) {
    uint8_t ie = bus_peek(c->bus, 0xFFFF);
    uint8_t if_reg = bus_peek(c->bus, 0xFF0F);
    if ((ie & if_reg & 0x1F) != 0) {
      c->halted = false;
    } else {
      sys_tick(c->bus, 1);
      return;
    }
  }

  cpu_execute_instruction(c);
}

void cpu_execute_instruction(CPU* c) {
  // Snapshot IME state for EI delay.
  bool ime_was_enabling = (c->ime == IME_ENABLING);

  // Opcode fetch. Normally this is fetch8(c) (read at PC, PC++).
  // If the HALT bug is pending, the read happens but PC does NOT
  // advance, causing the byte at PC to be consumed twice -- once
  // here as the post-HALT opcode, then again by the dispatched
  // instruction's own first fetch (or as itself, if it has no
  // operand).
  uint8_t opcode = bus_read(c->bus, c->pc);
  if (c->halt_bug_pending) {
    c->halt_bug_pending = false;
  } else {
    c->pc++;
  }

  if (opcode == 0xCB) {
    uint8_t sub = fetch8(c);
    dispatch(c, cb_ops, sub, true);
  } else {
    dispatch(c, ops, opcode, false);
  }

  // Promote ENABLING -> ENABLED only if the instruction did not
  // already change IME to something else (e.g., DI sets DISABLED;
  // RETI sets ENABLED directly). Without this guard, "EI; DI"
  // would incorrectly leave IME enabled.
  if (ime_was_enabling && c->ime == IME_ENABLING) {
    c->ime = IME_ENABLED;
  }
}

// Dispatch one opcode through a table. Aborts if the slot is empty,
// which during development means the opcode is not yet implemented.
static void dispatch(CPU* c, const OpFn* table, uint8_t opcode, bool cb) {
  OpFn fn = table[opcode];
  if (fn == NULL) {
    fprintf(stderr, "Unimplemented opcode: %s0x%02X\n", cb ? "CB " : "", opcode);
    abort();
  }
  fn(c, opcode);
}

// --- Interrupt servicing ---

/**
 * @brief Service a pending interrupt.
 *
 * Performs the 5 M-cycle interrupt entry sequence:
 *
 *   M1-M2: Two internal cycles.
 *   M3:    Push PC high byte to (--SP).
 *   M4:    Push PC low byte to (--SP).
 *   M5:    Read IE & IF, choose vector, clear the serviced IF bit,
 *          load PC. Disables IME and clears halted.
 *
 * IE/IF are read after the pushes, which can cancel or change the
 * serviced interrupt; see decisions.md 017.
 *
 * @pre c != NULL. Caller (cpu_step) has verified IME is enabled and
 *      at least one bit of IE & IF was set at entry.
 */
static void service_interrupt(CPU* c) {
  assert(c != NULL);

  // M1, M2: two internal cycles.
  sys_tick(c->bus, 1);
  sys_tick(c->bus, 1);

  // M3, M4: push PC high then low.
  bus_write(c->bus, --c->sp, (uint8_t)(c->pc >> 8));
  bus_write(c->bus, --c->sp, (uint8_t)(c->pc & 0xFF));

  // M5: read IE & IF after pushes, determine vector.
  uint8_t ie      = bus_peek(c->bus, 0xFFFF);
  uint8_t if_reg  = bus_peek(c->bus, 0xFF0F);
  uint8_t pending = ie & if_reg & 0x1F;

  uint16_t vector;
  if (pending == 0) {
    // Cancelled by IE overwrite during high-byte push.
    vector = 0x0000;
  } else {
    // Lowest-numbered set bit wins.
    // Priority order: VBlank, LCD STAT, Timer, Serial, Joypad.
    int bit = 0;
    while ((pending & (1u << bit)) == 0) bit++;
    vector = 0x0040 + (uint16_t)(bit * 8);

    // Clear only the serviced interrupt's IF bit.
    bus_poke(c->bus, 0xFF0F, (uint8_t)(if_reg & ~(1u << bit)));
  }

  c->pc     = vector;
  c->ime    = IME_DISABLED;
  c->halted = false;

  // M5 vector-setup cycle elapses.
  sys_tick(c->bus, 1);
}

// --- Dispatch tables ---
//
// Unfilled slots are NULL (zero-init). dispatch() aborts with the
// offending opcode if it hits a NULL slot. As opcodes are implemented
// in cpu_ops.c, add their entries here.

static const OpFn ops[256] = {
  [0x00] = op_nop,
  [0x06] = op_ld_b_n,
  [0x0E] = op_ld_c_n,
  [0x16] = op_ld_d_n,
  [0x1E] = op_ld_e_n,
  [0x26] = op_ld_h_n,
  [0x2E] = op_ld_l_n,
  [0x36] = op_ld_mhl_n, // LD (HL), n -- the missing index-6 case
  [0x3E] = op_ld_a_n,
  [0x01] = op_ld_bc_nn,
  [0x11] = op_ld_de_nn,
  [0x21] = op_ld_hl_nn,
  [0x31] = op_ld_sp_nn,

  // INC r / DEC r (8-bit). Destination encoded in bits 5:3, op in bit 0.
  // Index 6 = (HL): read/modify/write at memory pointed to by HL.
  [0x04] = op_inc_dec_r, [0x05] = op_inc_dec_r,  // INC B,  DEC B
  [0x0C] = op_inc_dec_r, [0x0D] = op_inc_dec_r,  // INC C,  DEC C
  [0x14] = op_inc_dec_r, [0x15] = op_inc_dec_r,  // INC D,  DEC D
  [0x1C] = op_inc_dec_r, [0x1D] = op_inc_dec_r,  // INC E,  DEC E
  [0x24] = op_inc_dec_r, [0x25] = op_inc_dec_r,  // INC H,  DEC H
  [0x2C] = op_inc_dec_r, [0x2D] = op_inc_dec_r,  // INC L,  DEC L
  [0x34] = op_inc_dec_r, [0x35] = op_inc_dec_r,  // INC (HL), DEC (HL)
  [0x3C] = op_inc_dec_r, [0x3D] = op_inc_dec_r,  // INC A,  DEC A

  // INC rr / DEC rr (16-bit). Pair in bits 5:4, op in bit 3.
  // No flag effects; 2 M-cycles each (1 internal beyond the fetch).
  [0x03] = op_inc_dec_rr, [0x0B] = op_inc_dec_rr,  // INC BC, DEC BC
  [0x13] = op_inc_dec_rr, [0x1B] = op_inc_dec_rr,  // INC DE, DEC DE
  [0x23] = op_inc_dec_rr, [0x2B] = op_inc_dec_rr,  // INC HL, DEC HL
  [0x33] = op_inc_dec_rr, [0x3B] = op_inc_dec_rr,  // INC SP, DEC SP

  // ADD HL, rr (16-bit). Source pair in bits 5:4. Z preserved;
  // N=0, H=carry from bit 11, C=carry from bit 15. 2 M-cycles each.
  [0x09] = op_add_hl_rr,  // ADD HL, BC
  [0x19] = op_add_hl_rr,  // ADD HL, DE
  [0x29] = op_add_hl_rr,  // ADD HL, HL
  [0x39] = op_add_hl_rr,  // ADD HL, SP

  // JR e and JR cc, e. Signed 8-bit relative jumps.
  // Unconditional is 3 M-cycles; conditional is 2 (not taken) or 3 (taken).
  [0x18] = op_jr_e,                                 // JR e (unconditional)
  [0x20] = op_jr_cc_e, [0x28] = op_jr_cc_e,         // JR NZ, e ; JR Z, e
  [0x30] = op_jr_cc_e, [0x38] = op_jr_cc_e,         // JR NC, e ; JR C, e

  // JP nn / JP cc, nn / JP HL. Absolute jumps.
  // JP nn is 4 M-cycles; JP cc, nn is 3 (not taken) or 4 (taken);
  // JP HL is 1 M-cycle (no internal cycle, no memory access).
  [0xC3] = op_jp_nn,                                // JP nn (unconditional)
  [0xC2] = op_jp_cc_nn, [0xCA] = op_jp_cc_nn,       // JP NZ, nn ; JP Z, nn
  [0xD2] = op_jp_cc_nn, [0xDA] = op_jp_cc_nn,       // JP NC, nn ; JP C, nn
  [0xE9] = op_jp_hl,                                // JP HL

  // CALL nn / CALL cc, nn. Subroutine calls with PC push.
  // Unconditional is 6 M-cycles; conditional is 3 (not taken) or 6 (taken).
  [0xCD] = op_call_nn,                              // CALL nn (unconditional)
  [0xC4] = op_call_cc_nn, [0xCC] = op_call_cc_nn,   // CALL NZ, nn ; CALL Z, nn
  [0xD4] = op_call_cc_nn, [0xDC] = op_call_cc_nn,   // CALL NC, nn ; CALL C, nn

  // RET / RET cc / RETI. Subroutine return.
  // RET is 4 M-cycles; RET cc is 2 (not taken) or 5 (taken) -- the
  // taken count is one MORE than unconditional RET because of the
  // condition-check internal cycle. RETI is 4 M-cycles and also
  // sets IME = ENABLED immediately.
  [0xC9] = op_ret,                                  // RET (unconditional)
  [0xC0] = op_ret_cc, [0xC8] = op_ret_cc,           // RET NZ ; RET Z
  [0xD0] = op_ret_cc, [0xD8] = op_ret_cc,           // RET NC ; RET C
  [0xD9] = op_reti,                                 // RETI

  // PUSH rr / POP rr. Stack ops on register pairs.
  // Pair encoding here is BC/DE/HL/AF (NOT SP at index 3).
  // PUSH is 4 M-cycles; POP is 3. POP AF masks F's low nibble to 0.
  [0xC5] = op_push_rr, [0xD5] = op_push_rr,         // PUSH BC ; PUSH DE
  [0xE5] = op_push_rr, [0xF5] = op_push_rr,         // PUSH HL ; PUSH AF
  [0xC1] = op_pop_rr,  [0xD1] = op_pop_rr,          // POP  BC ; POP  DE
  [0xE1] = op_pop_rr,  [0xF1] = op_pop_rr,          // POP  HL ; POP  AF

  // RST n. Single-byte CALL to hardcoded target (opcode & 0x38).
  // 4 M-cycles each. Same push semantics as CALL nn.
  [0xC7] = op_rst, [0xCF] = op_rst,                 // RST 0x00 ; RST 0x08
  [0xD7] = op_rst, [0xDF] = op_rst,                 // RST 0x10 ; RST 0x18
  [0xE7] = op_rst, [0xEF] = op_rst,                 // RST 0x20 ; RST 0x28
  [0xF7] = op_rst, [0xFF] = op_rst,                 // RST 0x30 ; RST 0x38

  // Accumulator rotates: RLCA, RRCA, RLA, RRA. 1 M-cycle each.
  // Z is forced to 0 (NOT computed from result) -- different from the
  // CB-prefix RLC/RRC/RL/RR. Op selector is opcode bits 4:3.
  [0x07] = op_rotate_a,  // RLCA
  [0x0F] = op_rotate_a,  // RRCA
  [0x17] = op_rotate_a,  // RLA
  [0x1F] = op_rotate_a,  // RRA

  // Flag/A singletons. 1 M-cycle each.
  [0x27] = op_daa,       // DAA: BCD adjust A. Preserves N, clears H.
  [0x2F] = op_cpl,       // CPL: A = ~A. Sets N=H=1, preserves Z and C.
  [0x37] = op_scf,       // SCF: C=1, clears N=H=0, preserves Z.
  [0x3F] = op_ccf,       // CCF: C=~C, clears N=H=0, preserves Z.

  // Interrupt and power control singletons.
  [0xF3] = op_di,        // DI: IME = DISABLED, immediate.
  [0xFB] = op_ei,        // EI: IME = ENABLING (commits at end of next instr).
  [0x76] = op_halt,      // HALT: suspend or trigger HALT bug. See decision 014.
  [0x10] = op_stop,      // STOP: 1-byte stub (per SingleStepTests).

  // Indirect loads to/from A through register pairs. 2 M-cycles each.
  [0x02] = op_ld_mbc_a,  // LD (BC), A
  [0x12] = op_ld_mde_a,  // LD (DE), A
  [0x0A] = op_ld_a_mbc,  // LD A, (BC)
  [0x1A] = op_ld_a_mde,  // LD A, (DE)
  [0x22] = op_ld_mhli_a, // LD (HL+), A  (HL post-increments)
  [0x32] = op_ld_mhld_a, // LD (HL-), A  (HL post-decrements)
  [0x2A] = op_ld_a_mhli, // LD A, (HL+)
  [0x3A] = op_ld_a_mhld, // LD A, (HL-)

  // LDH: high-page 0xFF00 accesses. n-immediate variants are 3 M-cycles,
  // C-indirect variants are 2 M-cycles.
  [0xE0] = op_ldh_n_a,   // LDH (n), A   addr = 0xFF00 | n
  [0xF0] = op_ldh_a_n,   // LDH A, (n)
  [0xE2] = op_ldh_mc_a,  // LDH (C), A   addr = 0xFF00 | C
  [0xF2] = op_ldh_a_mc,  // LDH A, (C)

  // Absolute 16-bit address loads. 4 M-cycles each (opcode + nn + memory).
  [0xEA] = op_ld_mnn_a,  // LD (nn), A
  [0xFA] = op_ld_a_mnn,  // LD A, (nn)

  // 16-bit at memory and SP/HL specials.
  [0x08] = op_ld_mnn_sp, // LD (nn), SP    5 M-cycles. Writes SP little-endian.
  [0xF9] = op_ld_sp_hl,  // LD SP, HL      2 M-cycles.
  [0xF8] = op_ld_hl_sp_e,// LD HL, SP+e    3 M-cycles. Z=N=0; H, C from low-byte add.
  [0xE8] = op_add_sp_e,  // ADD SP, e      4 M-cycles. Same flag rule as F8.

  // LD r, r' (0x40-0x7F minus 0x76 HALT). All dispatch to op_ld_r_r.
  [0x40] = op_ld_r_r, [0x41] = op_ld_r_r, [0x42] = op_ld_r_r, [0x43] = op_ld_r_r,
  [0x44] = op_ld_r_r, [0x45] = op_ld_r_r, [0x46] = op_ld_r_r, [0x47] = op_ld_r_r,
  [0x48] = op_ld_r_r, [0x49] = op_ld_r_r, [0x4A] = op_ld_r_r, [0x4B] = op_ld_r_r,
  [0x4C] = op_ld_r_r, [0x4D] = op_ld_r_r, [0x4E] = op_ld_r_r, [0x4F] = op_ld_r_r,
  [0x50] = op_ld_r_r, [0x51] = op_ld_r_r, [0x52] = op_ld_r_r, [0x53] = op_ld_r_r,
  [0x54] = op_ld_r_r, [0x55] = op_ld_r_r, [0x56] = op_ld_r_r, [0x57] = op_ld_r_r,
  [0x58] = op_ld_r_r, [0x59] = op_ld_r_r, [0x5A] = op_ld_r_r, [0x5B] = op_ld_r_r,
  [0x5C] = op_ld_r_r, [0x5D] = op_ld_r_r, [0x5E] = op_ld_r_r, [0x5F] = op_ld_r_r,
  [0x60] = op_ld_r_r, [0x61] = op_ld_r_r, [0x62] = op_ld_r_r, [0x63] = op_ld_r_r,
  [0x64] = op_ld_r_r, [0x65] = op_ld_r_r, [0x66] = op_ld_r_r, [0x67] = op_ld_r_r,
  [0x68] = op_ld_r_r, [0x69] = op_ld_r_r, [0x6A] = op_ld_r_r, [0x6B] = op_ld_r_r,
  [0x6C] = op_ld_r_r, [0x6D] = op_ld_r_r, [0x6E] = op_ld_r_r, [0x6F] = op_ld_r_r,
  [0x70] = op_ld_r_r, [0x71] = op_ld_r_r, [0x72] = op_ld_r_r, [0x73] = op_ld_r_r,
  [0x74] = op_ld_r_r, [0x75] = op_ld_r_r, /* 0x76 HALT */ [0x77] = op_ld_r_r,
  [0x78] = op_ld_r_r, [0x79] = op_ld_r_r, [0x7A] = op_ld_r_r, [0x7B] = op_ld_r_r,
  [0x7C] = op_ld_r_r, [0x7D] = op_ld_r_r, [0x7E] = op_ld_r_r, [0x7F] = op_ld_r_r,

  // 8-bit ALU on A (0x80-0xBF). All dispatch to op_alu_a_r.
  [0x80] = op_alu_a_r, [0x81] = op_alu_a_r, [0x82] = op_alu_a_r, [0x83] = op_alu_a_r,
  [0x84] = op_alu_a_r, [0x85] = op_alu_a_r, [0x86] = op_alu_a_r, [0x87] = op_alu_a_r,
  [0x88] = op_alu_a_r, [0x89] = op_alu_a_r, [0x8A] = op_alu_a_r, [0x8B] = op_alu_a_r,
  [0x8C] = op_alu_a_r, [0x8D] = op_alu_a_r, [0x8E] = op_alu_a_r, [0x8F] = op_alu_a_r,
  [0x90] = op_alu_a_r, [0x91] = op_alu_a_r, [0x92] = op_alu_a_r, [0x93] = op_alu_a_r,
  [0x94] = op_alu_a_r, [0x95] = op_alu_a_r, [0x96] = op_alu_a_r, [0x97] = op_alu_a_r,
  [0x98] = op_alu_a_r, [0x99] = op_alu_a_r, [0x9A] = op_alu_a_r, [0x9B] = op_alu_a_r,
  [0x9C] = op_alu_a_r, [0x9D] = op_alu_a_r, [0x9E] = op_alu_a_r, [0x9F] = op_alu_a_r,
  [0xA0] = op_alu_a_r, [0xA1] = op_alu_a_r, [0xA2] = op_alu_a_r, [0xA3] = op_alu_a_r,
  [0xA4] = op_alu_a_r, [0xA5] = op_alu_a_r, [0xA6] = op_alu_a_r, [0xA7] = op_alu_a_r,
  [0xA8] = op_alu_a_r, [0xA9] = op_alu_a_r, [0xAA] = op_alu_a_r, [0xAB] = op_alu_a_r,
  [0xAC] = op_alu_a_r, [0xAD] = op_alu_a_r, [0xAE] = op_alu_a_r, [0xAF] = op_alu_a_r,
  [0xB0] = op_alu_a_r, [0xB1] = op_alu_a_r, [0xB2] = op_alu_a_r, [0xB3] = op_alu_a_r,
  [0xB4] = op_alu_a_r, [0xB5] = op_alu_a_r, [0xB6] = op_alu_a_r, [0xB7] = op_alu_a_r,
  [0xB8] = op_alu_a_r, [0xB9] = op_alu_a_r, [0xBA] = op_alu_a_r, [0xBB] = op_alu_a_r,
  [0xBC] = op_alu_a_r, [0xBD] = op_alu_a_r, [0xBE] = op_alu_a_r, [0xBF] = op_alu_a_r,

  // 8-bit ALU on A with immediate (one opcode per operation).
  [0xC6] = op_alu_a_n, // ADD A, n
  [0xCE] = op_alu_a_n, // ADC A, n
  [0xD6] = op_alu_a_n, // SUB A, n
  [0xDE] = op_alu_a_n, // SBC A, n
  [0xE6] = op_alu_a_n, // AND A, n
  [0xEE] = op_alu_a_n, // XOR A, n
  [0xF6] = op_alu_a_n, // OR  A, n
  [0xFE] = op_alu_a_n, // CP  A, n
};

static const OpFn cb_ops[256] = {
  // CB 0x00 - 0x3F: rotates and shifts.
  // 8 operations (RLC/RRC/RL/RR/SLA/SRA/SWAP/SRL) x 8 destinations
  // (B/C/D/E/H/L/(HL)/A) = 64 opcodes, all dispatched through
  // op_cb_rotate_shift which decodes op (bits 5:3) and dst (bits 2:0).
  [0x00] = op_cb_rotate_shift, [0x01] = op_cb_rotate_shift,
  [0x02] = op_cb_rotate_shift, [0x03] = op_cb_rotate_shift,
  [0x04] = op_cb_rotate_shift, [0x05] = op_cb_rotate_shift,
  [0x06] = op_cb_rotate_shift, [0x07] = op_cb_rotate_shift,
  [0x08] = op_cb_rotate_shift, [0x09] = op_cb_rotate_shift,
  [0x0A] = op_cb_rotate_shift, [0x0B] = op_cb_rotate_shift,
  [0x0C] = op_cb_rotate_shift, [0x0D] = op_cb_rotate_shift,
  [0x0E] = op_cb_rotate_shift, [0x0F] = op_cb_rotate_shift,
  [0x10] = op_cb_rotate_shift, [0x11] = op_cb_rotate_shift,
  [0x12] = op_cb_rotate_shift, [0x13] = op_cb_rotate_shift,
  [0x14] = op_cb_rotate_shift, [0x15] = op_cb_rotate_shift,
  [0x16] = op_cb_rotate_shift, [0x17] = op_cb_rotate_shift,
  [0x18] = op_cb_rotate_shift, [0x19] = op_cb_rotate_shift,
  [0x1A] = op_cb_rotate_shift, [0x1B] = op_cb_rotate_shift,
  [0x1C] = op_cb_rotate_shift, [0x1D] = op_cb_rotate_shift,
  [0x1E] = op_cb_rotate_shift, [0x1F] = op_cb_rotate_shift,
  [0x20] = op_cb_rotate_shift, [0x21] = op_cb_rotate_shift,
  [0x22] = op_cb_rotate_shift, [0x23] = op_cb_rotate_shift,
  [0x24] = op_cb_rotate_shift, [0x25] = op_cb_rotate_shift,
  [0x26] = op_cb_rotate_shift, [0x27] = op_cb_rotate_shift,
  [0x28] = op_cb_rotate_shift, [0x29] = op_cb_rotate_shift,
  [0x2A] = op_cb_rotate_shift, [0x2B] = op_cb_rotate_shift,
  [0x2C] = op_cb_rotate_shift, [0x2D] = op_cb_rotate_shift,
  [0x2E] = op_cb_rotate_shift, [0x2F] = op_cb_rotate_shift,
  [0x30] = op_cb_rotate_shift, [0x31] = op_cb_rotate_shift,
  [0x32] = op_cb_rotate_shift, [0x33] = op_cb_rotate_shift,
  [0x34] = op_cb_rotate_shift, [0x35] = op_cb_rotate_shift,
  [0x36] = op_cb_rotate_shift, [0x37] = op_cb_rotate_shift,
  [0x38] = op_cb_rotate_shift, [0x39] = op_cb_rotate_shift,
  [0x3A] = op_cb_rotate_shift, [0x3B] = op_cb_rotate_shift,
  [0x3C] = op_cb_rotate_shift, [0x3D] = op_cb_rotate_shift,
  [0x3E] = op_cb_rotate_shift, [0x3F] = op_cb_rotate_shift,

  // CB 0x40 - 0x7F: BIT n, r. 64 opcodes. Bit n in bits 5:3, register
  // in bits 2:0. Flag rule traps: H is forced to 1 (NOT 0), C is preserved.
  [0x40] = op_cb_bit, [0x41] = op_cb_bit, [0x42] = op_cb_bit, [0x43] = op_cb_bit,
  [0x44] = op_cb_bit, [0x45] = op_cb_bit, [0x46] = op_cb_bit, [0x47] = op_cb_bit,
  [0x48] = op_cb_bit, [0x49] = op_cb_bit, [0x4A] = op_cb_bit, [0x4B] = op_cb_bit,
  [0x4C] = op_cb_bit, [0x4D] = op_cb_bit, [0x4E] = op_cb_bit, [0x4F] = op_cb_bit,
  [0x50] = op_cb_bit, [0x51] = op_cb_bit, [0x52] = op_cb_bit, [0x53] = op_cb_bit,
  [0x54] = op_cb_bit, [0x55] = op_cb_bit, [0x56] = op_cb_bit, [0x57] = op_cb_bit,
  [0x58] = op_cb_bit, [0x59] = op_cb_bit, [0x5A] = op_cb_bit, [0x5B] = op_cb_bit,
  [0x5C] = op_cb_bit, [0x5D] = op_cb_bit, [0x5E] = op_cb_bit, [0x5F] = op_cb_bit,
  [0x60] = op_cb_bit, [0x61] = op_cb_bit, [0x62] = op_cb_bit, [0x63] = op_cb_bit,
  [0x64] = op_cb_bit, [0x65] = op_cb_bit, [0x66] = op_cb_bit, [0x67] = op_cb_bit,
  [0x68] = op_cb_bit, [0x69] = op_cb_bit, [0x6A] = op_cb_bit, [0x6B] = op_cb_bit,
  [0x6C] = op_cb_bit, [0x6D] = op_cb_bit, [0x6E] = op_cb_bit, [0x6F] = op_cb_bit,
  [0x70] = op_cb_bit, [0x71] = op_cb_bit, [0x72] = op_cb_bit, [0x73] = op_cb_bit,
  [0x74] = op_cb_bit, [0x75] = op_cb_bit, [0x76] = op_cb_bit, [0x77] = op_cb_bit,
  [0x78] = op_cb_bit, [0x79] = op_cb_bit, [0x7A] = op_cb_bit, [0x7B] = op_cb_bit,
  [0x7C] = op_cb_bit, [0x7D] = op_cb_bit, [0x7E] = op_cb_bit, [0x7F] = op_cb_bit,

  // CB 0x80 - 0xBF: RES n, r. 64 opcodes. Flags preserved.
  [0x80] = op_cb_res, [0x81] = op_cb_res, [0x82] = op_cb_res, [0x83] = op_cb_res,
  [0x84] = op_cb_res, [0x85] = op_cb_res, [0x86] = op_cb_res, [0x87] = op_cb_res,
  [0x88] = op_cb_res, [0x89] = op_cb_res, [0x8A] = op_cb_res, [0x8B] = op_cb_res,
  [0x8C] = op_cb_res, [0x8D] = op_cb_res, [0x8E] = op_cb_res, [0x8F] = op_cb_res,
  [0x90] = op_cb_res, [0x91] = op_cb_res, [0x92] = op_cb_res, [0x93] = op_cb_res,
  [0x94] = op_cb_res, [0x95] = op_cb_res, [0x96] = op_cb_res, [0x97] = op_cb_res,
  [0x98] = op_cb_res, [0x99] = op_cb_res, [0x9A] = op_cb_res, [0x9B] = op_cb_res,
  [0x9C] = op_cb_res, [0x9D] = op_cb_res, [0x9E] = op_cb_res, [0x9F] = op_cb_res,
  [0xA0] = op_cb_res, [0xA1] = op_cb_res, [0xA2] = op_cb_res, [0xA3] = op_cb_res,
  [0xA4] = op_cb_res, [0xA5] = op_cb_res, [0xA6] = op_cb_res, [0xA7] = op_cb_res,
  [0xA8] = op_cb_res, [0xA9] = op_cb_res, [0xAA] = op_cb_res, [0xAB] = op_cb_res,
  [0xAC] = op_cb_res, [0xAD] = op_cb_res, [0xAE] = op_cb_res, [0xAF] = op_cb_res,
  [0xB0] = op_cb_res, [0xB1] = op_cb_res, [0xB2] = op_cb_res, [0xB3] = op_cb_res,
  [0xB4] = op_cb_res, [0xB5] = op_cb_res, [0xB6] = op_cb_res, [0xB7] = op_cb_res,
  [0xB8] = op_cb_res, [0xB9] = op_cb_res, [0xBA] = op_cb_res, [0xBB] = op_cb_res,
  [0xBC] = op_cb_res, [0xBD] = op_cb_res, [0xBE] = op_cb_res, [0xBF] = op_cb_res,

  // CB 0xC0 - 0xFF: SET n, r. 64 opcodes. Flags preserved.
  [0xC0] = op_cb_set, [0xC1] = op_cb_set, [0xC2] = op_cb_set, [0xC3] = op_cb_set,
  [0xC4] = op_cb_set, [0xC5] = op_cb_set, [0xC6] = op_cb_set, [0xC7] = op_cb_set,
  [0xC8] = op_cb_set, [0xC9] = op_cb_set, [0xCA] = op_cb_set, [0xCB] = op_cb_set,
  [0xCC] = op_cb_set, [0xCD] = op_cb_set, [0xCE] = op_cb_set, [0xCF] = op_cb_set,
  [0xD0] = op_cb_set, [0xD1] = op_cb_set, [0xD2] = op_cb_set, [0xD3] = op_cb_set,
  [0xD4] = op_cb_set, [0xD5] = op_cb_set, [0xD6] = op_cb_set, [0xD7] = op_cb_set,
  [0xD8] = op_cb_set, [0xD9] = op_cb_set, [0xDA] = op_cb_set, [0xDB] = op_cb_set,
  [0xDC] = op_cb_set, [0xDD] = op_cb_set, [0xDE] = op_cb_set, [0xDF] = op_cb_set,
  [0xE0] = op_cb_set, [0xE1] = op_cb_set, [0xE2] = op_cb_set, [0xE3] = op_cb_set,
  [0xE4] = op_cb_set, [0xE5] = op_cb_set, [0xE6] = op_cb_set, [0xE7] = op_cb_set,
  [0xE8] = op_cb_set, [0xE9] = op_cb_set, [0xEA] = op_cb_set, [0xEB] = op_cb_set,
  [0xEC] = op_cb_set, [0xED] = op_cb_set, [0xEE] = op_cb_set, [0xEF] = op_cb_set,
  [0xF0] = op_cb_set, [0xF1] = op_cb_set, [0xF2] = op_cb_set, [0xF3] = op_cb_set,
  [0xF4] = op_cb_set, [0xF5] = op_cb_set, [0xF6] = op_cb_set, [0xF7] = op_cb_set,
  [0xF8] = op_cb_set, [0xF9] = op_cb_set, [0xFA] = op_cb_set, [0xFB] = op_cb_set,
  [0xFC] = op_cb_set, [0xFD] = op_cb_set, [0xFE] = op_cb_set, [0xFF] = op_cb_set,
};