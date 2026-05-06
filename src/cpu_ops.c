/**
 * @file cpu_ops.c
 * @brief Opcode implementations for the SM83 CPU.
 *
 * All opcode functions are dispatched via the ops[] and cb_ops[]
 * tables defined in cpu.c. Functions follow the OpFn signature
 * (CPU*, uint8_t opcode); decode-based handlers extract register
 * and operation indices from the opcode byte, while single-purpose
 * handlers cast it to (void).
 *
 * Cycle accounting: every memory access flows through bus_read or
 * bus_write, each of which advances the system clock by 1 M-cycle
 * via sys_tick. Internal cycles (no memory access) are spent by
 * calling sys_tick directly. The opcode-fetch M-cycle is consumed
 * by cpu_step before the opcode function is called, so opcode
 * bodies account only for the additional cycles their execution
 * requires.
 *
 * Unimplemented opcodes are not represented here; the dispatch
 * function in cpu.c aborts when a table slot is NULL.
 */

#include "cpu_internal.h"

// --- Register index decoding ---
//
// The 3-bit register encoding used by many opcode families:
//   0=B, 1=C, 2=D, 3=E, 4=H, 5=L, 6=(HL), 7=A
//
// Index 6 means "the byte at address HL" (memory access), not a
// register. Helpers that work with this encoding handle index 6
// specially.

/**
 * @brief Read the 8-bit value selected by a 3-bit index.
 *
 * For indices 0-5 and 7, returns the value of the named register.
 * For index 6, performs a memory read at HL (1 M-cycle).
 */
static uint8_t read_r8(CPU* c, int idx) {
  switch (idx) {
    case 0: return c->r.b;
    case 1: return c->r.c;
    case 2: return c->r.d;
    case 3: return c->r.e;
    case 4: return c->r.h;
    case 5: return c->r.l;
    case 6: return bus_read(c->bus, reg_get_hl(&c->r));
    case 7: return c->r.a;
  }
  return 0; // unreachable; idx is always 0-7
}

/**
 * @brief Write an 8-bit value to the location selected by a 3-bit index.
 *
 * For indices 0-5 and 7, writes the named register. For index 6,
 * performs a memory write at HL (1 M-cycle).
 */
static void write_r8(CPU* c, int idx, uint8_t v) {
  switch (idx) {
    case 0: c->r.b = v; break;
    case 1: c->r.c = v; break;
    case 2: c->r.d = v; break;
    case 3: c->r.e = v; break;
    case 4: c->r.h = v; break;
    case 5: c->r.l = v; break;
    case 6: bus_write(c->bus, reg_get_hl(&c->r), v); break;
    case 7: c->r.a = v; break;
  }
}

// --- Flag helpers ---
//
// Set or clear a single flag bit in F. F's low nibble is always 0.

static inline void set_flag(CPU* c, uint8_t mask, bool on) {
  if (on) c->r.f |= mask;
  else    c->r.f &= (uint8_t)~mask;
}

// --- 8-bit ALU on A: shared computation ---
//
// Both op_alu_a_r (ADD/ADC/SUB/.../CP A,r) and op_alu_a_n (the same
// operations against an immediate byte) decode the same 3-bit
// operation selector from opcode bits 5:3 and apply the same
// arithmetic. The only difference between them is where the second
// operand comes from: a register/(HL) read versus an immediate
// fetch. alu_compute owns the arithmetic and the flag updates so
// the rules live in exactly one place.
//
// Operation selector:
//   0=ADD, 1=ADC, 2=SUB, 3=SBC, 4=AND, 5=XOR, 6=OR, 7=CP
//
// Flag rules:
//   ADD: Z=result==0, N=0, H=carry from bit 3, C=carry from bit 7
//   ADC: same as ADD but include carry-in
//   SUB: Z=result==0, N=1, H=borrow from bit 4, C=borrow from bit 8
//   SBC: same as SUB but include carry-in
//   AND: Z=result==0, N=0, H=1, C=0
//   XOR: Z=result==0, N=0, H=0, C=0
//   OR : Z=result==0, N=0, H=0, C=0
//   CP : same flags as SUB but A is not modified

/**
 * @brief Apply an 8-bit ALU operation to register A.
 *
 * @param op      Operation selector (0..7); see header comment above.
 * @param operand Second operand (already fetched; alu_compute does
 *                no memory access of its own).
 *
 * Updates A (except for CP) and all four flag bits.
 */
static void alu_compute(CPU* c, int op, uint8_t operand) {
  uint8_t a = c->r.a;
  uint8_t carry_in = (c->r.f & FLAG_C) ? 1 : 0;
  uint8_t result;

  switch (op) {
    case 0: { // ADD
      uint16_t wide = (uint16_t)a + (uint16_t)operand;
      result = (uint8_t)wide;
      set_flag(c, FLAG_Z, result == 0);
      set_flag(c, FLAG_N, false);
      set_flag(c, FLAG_H, ((a & 0x0F) + (operand & 0x0F)) > 0x0F);
      set_flag(c, FLAG_C, wide > 0xFF);
      c->r.a = result;
      break;
    }
    case 1: { // ADC
      uint16_t wide = (uint16_t)a + (uint16_t)operand + (uint16_t)carry_in;
      result = (uint8_t)wide;
      set_flag(c, FLAG_Z, result == 0);
      set_flag(c, FLAG_N, false);
      set_flag(c, FLAG_H, ((a & 0x0F) + (operand & 0x0F) + carry_in) > 0x0F);
      set_flag(c, FLAG_C, wide > 0xFF);
      c->r.a = result;
      break;
    }
    case 2: { // SUB
      int wide = (int)a - (int)operand;
      result = (uint8_t)wide;
      set_flag(c, FLAG_Z, result == 0);
      set_flag(c, FLAG_N, true);
      set_flag(c, FLAG_H, ((int)(a & 0x0F) - (int)(operand & 0x0F)) < 0);
      set_flag(c, FLAG_C, wide < 0);
      c->r.a = result;
      break;
    }
    case 3: { // SBC
      int wide = (int)a - (int)operand - (int)carry_in;
      result = (uint8_t)wide;
      set_flag(c, FLAG_Z, result == 0);
      set_flag(c, FLAG_N, true);
      set_flag(c, FLAG_H, ((int)(a & 0x0F) - (int)(operand & 0x0F) - (int)carry_in) < 0);
      set_flag(c, FLAG_C, wide < 0);
      c->r.a = result;
      break;
    }
    case 4: { // AND
      result = a & operand;
      set_flag(c, FLAG_Z, result == 0);
      set_flag(c, FLAG_N, false);
      set_flag(c, FLAG_H, true);
      set_flag(c, FLAG_C, false);
      c->r.a = result;
      break;
    }
    case 5: { // XOR
      result = a ^ operand;
      set_flag(c, FLAG_Z, result == 0);
      set_flag(c, FLAG_N, false);
      set_flag(c, FLAG_H, false);
      set_flag(c, FLAG_C, false);
      c->r.a = result;
      break;
    }
    case 6: { // OR
      result = a | operand;
      set_flag(c, FLAG_Z, result == 0);
      set_flag(c, FLAG_N, false);
      set_flag(c, FLAG_H, false);
      set_flag(c, FLAG_C, false);
      c->r.a = result;
      break;
    }
    case 7: { // CP (SUB without writeback)
      int wide = (int)a - (int)operand;
      result = (uint8_t)wide;
      set_flag(c, FLAG_Z, result == 0);
      set_flag(c, FLAG_N, true);
      set_flag(c, FLAG_H, ((int)(a & 0x0F) - (int)(operand & 0x0F)) < 0);
      set_flag(c, FLAG_C, wide < 0);
      // A not modified
      break;
    }
  }
}

// --- Stack helpers ---

/**
 * @brief Pop a 16-bit value from the stack.
 *
 * Reads two bytes at SP (low first, then high) and increments SP
 * after each read. Spends 2 M-cycles via bus_read. Used by RET,
 * RET cc, RETI, and (later) POP rr.
 */
static uint16_t pop16(CPU* c) {
  uint8_t lo = bus_read(c->bus, c->sp++);
  uint8_t hi = bus_read(c->bus, c->sp++);
  return ((uint16_t)hi << 8) | lo;
}

// --- Misc ---

/**
 * @brief NOP (0x00). 1 M-cycle (opcode fetch only). No effect.
 */
void op_nop(CPU* c, uint8_t opcode) {
  (void)c;
  (void)opcode;
}

// --- LD r, n (8-bit immediate to register) ---
//
// Each is 2 M-cycles: opcode fetch (paid by cpu_step) + immediate fetch.

/** @brief LD B, n (0x06). */
void op_ld_b_n(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.b = fetch8(c);
}

/** @brief LD C, n (0x0E). */
void op_ld_c_n(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.c = fetch8(c);
}

/** @brief LD D, n (0x16). */
void op_ld_d_n(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.d = fetch8(c);
}

/** @brief LD E, n (0x1E). */
void op_ld_e_n(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.e = fetch8(c);
}

/** @brief LD H, n (0x26). */
void op_ld_h_n(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.h = fetch8(c);
}

/** @brief LD L, n (0x2E). */
void op_ld_l_n(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.l = fetch8(c);
}

/** @brief LD A, n (0x3E). */
void op_ld_a_n(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.a = fetch8(c);
}

// --- LD rr, nn (16-bit immediate to register pair) ---
//
// Each is 3 M-cycles: opcode fetch (paid by cpu_step) + 2-byte immediate fetch.

/** @brief LD BC, nn (0x01). */
void op_ld_bc_nn(CPU* c, uint8_t opcode) {
  (void)opcode;
  reg_set_bc(&c->r, fetch16(c));
}

/** @brief LD DE, nn (0x11). */
void op_ld_de_nn(CPU* c, uint8_t opcode) {
  (void)opcode;
  reg_set_de(&c->r, fetch16(c));
}

/** @brief LD HL, nn (0x21). */
void op_ld_hl_nn(CPU* c, uint8_t opcode) {
  (void)opcode;
  reg_set_hl(&c->r, fetch16(c));
}

/** @brief LD SP, nn (0x31). */
void op_ld_sp_nn(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->sp = fetch16(c);
}

/**
 * @brief LD r, r' family (0x40-0x7F minus 0x76 HALT).
 *
 * Cycles: 1 M-cycle when both operands are registers, 2 M-cycles when
 * either operand is (HL) (memory access). Source and destination are
 * decoded from opcode bits 2:0 and 5:3 respectively.
 */
void op_ld_r_r(CPU* c, uint8_t opcode) {
  int dst = (opcode >> 3) & 0x07;
  int src = opcode & 0x07;
  uint8_t v = read_r8(c, src);
  write_r8(c, dst, v);
}

/**
 * @brief 8-bit ALU on A, register/memory source (0x80-0xBF).
 *
 * Cycles: 1 M-cycle for register source, 2 M-cycles when source is (HL).
 * Operation selector is opcode bits 5:3; source is opcode bits 2:0.
 * See alu_compute for the per-operation flag rules.
 */
void op_alu_a_r(CPU* c, uint8_t opcode) {
  int op  = (opcode >> 3) & 0x07;
  int src = opcode & 0x07;
  uint8_t operand = read_r8(c, src);
  alu_compute(c, op, operand);
}

/**
 * @brief 8-bit ALU on A, immediate source (0xC6, 0xCE, 0xD6, 0xDE,
 *        0xE6, 0xEE, 0xF6, 0xFE).
 *
 * Cycles: 2 M-cycles total (opcode fetch + immediate fetch).
 * Operation selector is opcode bits 5:3, identical to op_alu_a_r:
 *   0xC6=ADD, 0xCE=ADC, 0xD6=SUB, 0xDE=SBC,
 *   0xE6=AND, 0xEE=XOR, 0xF6=OR,  0xFE=CP.
 */
void op_alu_a_n(CPU* c, uint8_t opcode) {
  int op = (opcode >> 3) & 0x07;
  uint8_t operand = fetch8(c);
  alu_compute(c, op, operand);
}

/**
 * @brief INC r / DEC r family (0x04, 0x05, 0x0C, 0x0D, 0x14, 0x15,
 *        0x1C, 0x1D, 0x24, 0x25, 0x2C, 0x2D, 0x34, 0x35, 0x3C, 0x3D).
 *
 * Destination is opcode bits 5:3 (standard 3-bit register encoding,
 * including (HL) at index 6). Operation is opcode bit 0: 0=INC, 1=DEC.
 *
 * Flag rules:
 *   INC: Z=result==0, N=0, H=carry from bit 3 (i.e. old low nibble was 0xF).
 *   DEC: Z=result==0, N=1, H=borrow from bit 4 (i.e. old low nibble was 0x0).
 *   C is preserved by both.
 *
 * Cycles: 1 M-cycle for register destination, 3 M-cycles for (HL)
 * (opcode fetch + memory read + memory write). The cycle count is
 * driven by read_r8 and write_r8, which tick the bus when accessing
 * (HL).
 */
void op_inc_dec_r(CPU* c, uint8_t opcode) {
  int dst = (opcode >> 3) & 0x07;
  bool is_dec = (opcode & 0x01) != 0;

  uint8_t old_val = read_r8(c, dst);
  uint8_t new_val = is_dec ? (uint8_t)(old_val - 1) : (uint8_t)(old_val + 1);

  set_flag(c, FLAG_Z, new_val == 0);
  set_flag(c, FLAG_N, is_dec);
  if (is_dec) {
    // Half-borrow: low nibble was 0x0 before the subtract.
    set_flag(c, FLAG_H, (old_val & 0x0F) == 0x00);
  } else {
    // Half-carry: low nibble was 0xF before the add.
    set_flag(c, FLAG_H, (old_val & 0x0F) == 0x0F);
  }
  // C is preserved.

  write_r8(c, dst, new_val);
}

/**
 * @brief INC rr / DEC rr family (0x03, 0x0B, 0x13, 0x1B, 0x23, 0x2B,
 *        0x33, 0x3B).
 *
 * Increments or decrements a 16-bit register pair (BC, DE, HL, SP).
 * The pair is selected by opcode bits 5:4:
 *   00=BC, 01=DE, 10=HL, 11=SP.
 * The operation is opcode bit 3: 0=INC, 1=DEC.
 *
 * Flags: none. All four flag bits (Z, N, H, C) are preserved. The
 * 8-bit INC/DEC family touches Z/N/H but the 16-bit family does
 * not -- this asymmetry is real hardware behavior, not an oversight.
 *
 * Cycles: 2 M-cycles (opcode fetch + 1 internal cycle for the
 * 16-bit ALU). The internal cycle is paid by an explicit sys_tick;
 * no memory access takes place during the operation itself.
 */
void op_inc_dec_rr(CPU* c, uint8_t opcode) {
  int pair   = (opcode >> 4) & 0x03;
  bool is_dec = (opcode & 0x08) != 0;

  uint16_t v;
  switch (pair) {
    case 0: v = reg_get_bc(&c->r); break;
    case 1: v = reg_get_de(&c->r); break;
    case 2: v = reg_get_hl(&c->r); break;
    case 3: v = c->sp;             break;
    default: v = 0; // unreachable
  }

  v = is_dec ? (uint16_t)(v - 1) : (uint16_t)(v + 1);

  switch (pair) {
    case 0: reg_set_bc(&c->r, v); break;
    case 1: reg_set_de(&c->r, v); break;
    case 2: reg_set_hl(&c->r, v); break;
    case 3: c->sp = v;            break;
  }

  // 1 internal M-cycle (16-bit ALU). No memory access, no flag changes.
  sys_tick(c->bus, 1);
}

/**
 * @brief ADD HL, rr family (0x09, 0x19, 0x29, 0x39).
 *
 * Adds a 16-bit register pair to HL and stores the result in HL.
 * Source pair is opcode bits 5:4: 00=BC, 01=DE, 10=HL, 11=SP.
 *
 * Flag rules (note the divergence from 8-bit ADD):
 *   Z: PRESERVED. Not recomputed from the result. This is the
 *      single most-failed corner of this family in test suites.
 *   N: 0
 *   H: carry from bit 11 to bit 12 of the 16-bit add.
 *   C: carry from bit 15 to bit 16 (i.e. unsigned overflow).
 *
 * Cycles: 2 M-cycles (opcode fetch + 1 internal for the 16-bit ALU).
 */
void op_add_hl_rr(CPU* c, uint8_t opcode) {
  int pair = (opcode >> 4) & 0x03;

  uint16_t hl = reg_get_hl(&c->r);
  uint16_t rr;
  switch (pair) {
    case 0: rr = reg_get_bc(&c->r); break;
    case 1: rr = reg_get_de(&c->r); break;
    case 2: rr = hl;                break;  // ADD HL, HL
    case 3: rr = c->sp;             break;
    default: rr = 0; // unreachable
  }

  uint32_t wide = (uint32_t)hl + (uint32_t)rr;

  // Z is preserved -- do not touch it.
  set_flag(c, FLAG_N, false);
  set_flag(c, FLAG_H, ((hl & 0x0FFF) + (rr & 0x0FFF)) > 0x0FFF);
  set_flag(c, FLAG_C, wide > 0xFFFF);

  reg_set_hl(&c->r, (uint16_t)wide);

  // 1 internal M-cycle (16-bit ALU).
  sys_tick(c->bus, 1);
}

// --- Condition codes ---
//
// Conditional control-flow opcodes (JR cc, JP cc, CALL cc, RET cc)
// share a 2-bit condition selector encoded in opcode bits 4:3:
//   00=NZ, 01=Z, 10=NC, 11=C.

/**
 * @brief Test whether a condition code is satisfied.
 *
 * @param cc Condition selector (0..3); see header comment above.
 */
static bool check_cc(const CPU* c, int cc) {
  switch (cc) {
    case 0: return !(c->r.f & FLAG_Z); // NZ
    case 1: return  (c->r.f & FLAG_Z); // Z
    case 2: return !(c->r.f & FLAG_C); // NC
    case 3: return  (c->r.f & FLAG_C); // C
  }
  return false; // unreachable
}

/**
 * @brief JR e (0x18). Unconditional relative jump.
 *
 * Fetches a signed 8-bit offset and adds it to PC. PC has already
 * advanced past the offset byte by the time the addition happens,
 * which is why "JR e=0" is a no-op (PC lands on the next instruction).
 * PC wraps at 16 bits.
 *
 * Cycles: 3 M-cycles (opcode fetch + immediate fetch + 1 internal
 * cycle for the PC update). Flags are unaffected.
 */
void op_jr_e(CPU* c, uint8_t opcode) {
  (void)opcode;
  int8_t offset = (int8_t)fetch8(c);
  c->pc = (uint16_t)(c->pc + offset);
  sys_tick(c->bus, 1); // internal cycle for the jump
}

/**
 * @brief JR cc, e family (0x20 NZ, 0x28 Z, 0x30 NC, 0x38 C).
 *
 * Conditional relative jump. Condition selector is opcode bits 4:3.
 *
 * The offset byte is fetched unconditionally -- consuming the byte
 * from the instruction stream is part of the opcode regardless of
 * whether the jump is taken. Only the PC update and its internal
 * cycle are conditional.
 *
 * Cycles: 2 M-cycles when not taken, 3 when taken. Flags unaffected.
 */
void op_jr_cc_e(CPU* c, uint8_t opcode) {
  int cc = (opcode >> 3) & 0x03;
  int8_t offset = (int8_t)fetch8(c); // always fetched

  if (check_cc(c, cc)) {
    c->pc = (uint16_t)(c->pc + offset);
    sys_tick(c->bus, 1); // internal cycle only on taken
  }
}

/**
 * @brief JP nn (0xC3). Unconditional absolute jump.
 *
 * Cycles: 4 M-cycles (opcode fetch + low byte + high byte + 1
 * internal cycle for the PC update). Flags unaffected.
 */
void op_jp_nn(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint16_t target = fetch16(c);
  c->pc = target;
  sys_tick(c->bus, 1); // internal cycle for the jump
}

/**
 * @brief JP cc, nn family (0xC2 NZ, 0xCA Z, 0xD2 NC, 0xDA C).
 *
 * Conditional absolute jump. Both target bytes are fetched
 * unconditionally; only the PC update and its internal cycle are
 * conditional.
 *
 * Cycles: 3 M-cycles when not taken, 4 when taken. Flags unaffected.
 */
void op_jp_cc_nn(CPU* c, uint8_t opcode) {
  int cc = (opcode >> 3) & 0x03;
  uint16_t target = fetch16(c); // always fetched

  if (check_cc(c, cc)) {
    c->pc = target;
    sys_tick(c->bus, 1); // internal cycle only on taken
  }
}

/**
 * @brief JP HL (0xE9). Jump to the address held in register HL.
 *
 * Despite the (HL) notation sometimes used in older docs, this is
 * NOT a memory access; PC simply takes the current value of HL.
 *
 * Cycles: 1 M-cycle (just the opcode fetch, paid by cpu_step). No
 * internal cycle -- there is no 16-bit ALU work to do because HL
 * is already a 16-bit value in registers. Flags unaffected.
 */
void op_jp_hl(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->pc = reg_get_hl(&c->r);
}

/**
 * @brief CALL nn (0xCD). Unconditional subroutine call.
 *
 * Fetches a 16-bit target, pushes the return address (PC after the
 * operand bytes -- i.e., the address of the instruction following
 * this CALL) onto the stack, and jumps to the target.
 *
 * The push is two writes, high byte first, with SP pre-decremented
 * each time. After execution, memory is laid out little-endian:
 * low byte at the lower address, matching fetch16's read order so
 * RET can pop back symmetrically.
 *
 * Cycles: 6 M-cycles total:
 *   1: opcode fetch (paid by cpu_step)
 *   2-3: low + high byte of target
 *   4: internal cycle (SP decrement, no memory access)
 *   5: write return-PC high byte to --SP
 *   6: write return-PC low  byte to --SP
 *
 * Flags: unaffected.
 */
void op_call_nn(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint16_t target = fetch16(c);

  // Internal cycle: SP decrement / push setup, no memory access.
  sys_tick(c->bus, 1);

  // Push return address (current PC) high byte then low byte.
  bus_write(c->bus, --c->sp, (uint8_t)(c->pc >> 8));
  bus_write(c->bus, --c->sp, (uint8_t)(c->pc & 0xFF));

  c->pc = target;
}

/**
 * @brief CALL cc, nn family (0xC4 NZ, 0xCC Z, 0xD4 NC, 0xDC C).
 *
 * Conditional subroutine call. Both target bytes are fetched
 * unconditionally; only the push and PC update happen on taken.
 *
 * Cycles: 3 M-cycles when not taken, 6 when taken. Flags unaffected.
 */
void op_call_cc_nn(CPU* c, uint8_t opcode) {
  int cc = (opcode >> 3) & 0x03;
  uint16_t target = fetch16(c); // always fetched

  if (check_cc(c, cc)) {
    // Internal cycle: SP decrement / push setup.
    sys_tick(c->bus, 1);

    bus_write(c->bus, --c->sp, (uint8_t)(c->pc >> 8));
    bus_write(c->bus, --c->sp, (uint8_t)(c->pc & 0xFF));

    c->pc = target;
  }
}

/**
 * @brief RET (0xC9). Unconditional return from subroutine.
 *
 * Pops a 16-bit return address from the stack and loads it into PC.
 *
 * Cycles: 4 M-cycles total:
 *   1: opcode fetch (paid by cpu_step)
 *   2: pop low byte from (SP++)
 *   3: pop high byte from (SP++)
 *   4: internal cycle for the PC update
 *
 * Flags: unaffected.
 */
void op_ret(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->pc = pop16(c);
  sys_tick(c->bus, 1); // internal cycle for PC update
}

/**
 * @brief RET cc family (0xC0 NZ, 0xC8 Z, 0xD0 NC, 0xD8 C).
 *
 * Conditional return. Note the cycle asymmetry: there is an
 * additional internal cycle at the *start* of the instruction for
 * the condition check, which makes RET cc taken cost 5 M-cycles --
 * one MORE than unconditional RET, not the same.
 *
 * Cycles:
 *   not taken (2 M-cycles): opcode fetch + condition-check internal.
 *   taken     (5 M-cycles): opcode fetch + condition-check internal
 *                           + pop low + pop high + PC-update internal.
 *
 * Flags: unaffected.
 */
void op_ret_cc(CPU* c, uint8_t opcode) {
  int cc = (opcode >> 3) & 0x03;

  // Condition-check internal cycle, always present.
  sys_tick(c->bus, 1);

  if (check_cc(c, cc)) {
    c->pc = pop16(c);
    sys_tick(c->bus, 1); // internal cycle for PC update
  }
}

/**
 * @brief RETI (0xD9). Return and enable interrupts.
 *
 * Identical to RET in timing and stack behavior, but additionally
 * sets IME to IME_ENABLED *immediately* -- there is no one-instruction
 * delay like EI has. After RETI, the very next interrupt check (at
 * the start of the following cpu_step) sees IME enabled.
 *
 * Cycles: 4 M-cycles (same as RET). Flags: unaffected.
 */
void op_reti(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->pc = pop16(c);
  sys_tick(c->bus, 1); // internal cycle for PC update
  c->ime = IME_ENABLED;
}

/**
 * @brief PUSH rr family (0xC5 BC, 0xD5 DE, 0xE5 HL, 0xF5 AF).
 *
 * Pushes a register pair onto the stack, high byte then low byte,
 * with SP pre-decremented each time (so memory ends up little-endian
 * with low byte at the lower address, matching CALL's push order).
 *
 * Pair encoding (opcode bits 5:4): 00=BC, 01=DE, 10=HL, 11=AF.
 * Note that index 3 selects AF here, NOT SP -- this differs from the
 * INC/DEC rr and ADD HL, rr families.
 *
 * For PUSH AF, reg_get_af masks F's low nibble to 0 before pushing,
 * ensuring the stack reflects real hardware (F's low 4 bits are
 * hardwired to zero).
 *
 * Cycles: 4 M-cycles total:
 *   1: opcode fetch (paid by cpu_step)
 *   2: internal cycle (SP decrement setup)
 *   3: write high byte at --SP
 *   4: write low  byte at --SP
 *
 * Flags: unaffected.
 */
void op_push_rr(CPU* c, uint8_t opcode) {
  int pair = (opcode >> 4) & 0x03;

  uint16_t v;
  switch (pair) {
    case 0: v = reg_get_bc(&c->r); break;
    case 1: v = reg_get_de(&c->r); break;
    case 2: v = reg_get_hl(&c->r); break;
    case 3: v = reg_get_af(&c->r); break;  // F low nibble masked
    default: v = 0; // unreachable
  }

  // Internal cycle for SP decrement / push setup.
  sys_tick(c->bus, 1);

  bus_write(c->bus, --c->sp, (uint8_t)(v >> 8));
  bus_write(c->bus, --c->sp, (uint8_t)(v & 0xFF));
}

/**
 * @brief POP rr family (0xC1 BC, 0xD1 DE, 0xE1 HL, 0xF1 AF).
 *
 * Pops a 16-bit value from the stack into a register pair. Read
 * order: low byte at SP, high byte at SP+1, with SP++ each time
 * (matches RET, which is also why CALL/RET round-trips work).
 *
 * Pair encoding (opcode bits 5:4): 00=BC, 01=DE, 10=HL, 11=AF.
 *
 * For POP AF, reg_set_af forces F's low nibble to 0. This is real
 * hardware behavior: even if the byte on the stack has nonzero low
 * bits, F never holds them. POP AF is also the ONLY opcode in this
 * family that affects flags -- it loads F from memory.
 *
 * Cycles: 3 M-cycles (opcode fetch + 2 reads via pop16).
 *
 * Flags: unaffected by POP BC/DE/HL. POP AF replaces F entirely
 * with the popped low byte (low nibble masked).
 */
void op_pop_rr(CPU* c, uint8_t opcode) {
  int pair = (opcode >> 4) & 0x03;

  uint16_t v = pop16(c);

  switch (pair) {
    case 0: reg_set_bc(&c->r, v); break;
    case 1: reg_set_de(&c->r, v); break;
    case 2: reg_set_hl(&c->r, v); break;
    case 3: reg_set_af(&c->r, v); break;  // F low nibble forced to 0
  }
}

/**
 * @brief RST n family (0xC7, 0xCF, 0xD7, 0xDF, 0xE7, 0xEF, 0xF7, 0xFF).
 *
 * Single-byte subroutine call to a hardcoded target. Targets are
 * encoded in opcode bits 5:3 as multiples of 8:
 *   0xC7 -> 0x00, 0xCF -> 0x08, 0xD7 -> 0x10, 0xDF -> 0x18,
 *   0xE7 -> 0x20, 0xEF -> 0x28, 0xF7 -> 0x30, 0xFF -> 0x38.
 *
 * Because bits 5:3 already sit in the right place to express n * 8,
 * the target is just (opcode & 0x38) -- no shift needed.
 *
 * Used for fast inline calls and (architecturally) shares the layout
 * of the interrupt vectors: VBlank=0x40, STAT=0x48, etc., are 8-byte
 * slots just past the RST range so the RST encoding could in
 * principle dispatch to them, though no opcode actually maps to those.
 *
 * Cycles: 4 M-cycles total, identical structure to CALL nn minus
 * the operand fetch:
 *   1: opcode fetch (paid by cpu_step)
 *   2: internal cycle (SP decrement setup)
 *   3: write return-PC high byte at --SP
 *   4: write return-PC low  byte at --SP
 *
 * Flags: unaffected.
 */
void op_rst(CPU* c, uint8_t opcode) {
  uint16_t target = (uint16_t)(opcode & 0x38);

  // Internal cycle for SP decrement / push setup.
  sys_tick(c->bus, 1);

  // Push return address (PC after the single-byte opcode).
  bus_write(c->bus, --c->sp, (uint8_t)(c->pc >> 8));
  bus_write(c->bus, --c->sp, (uint8_t)(c->pc & 0xFF));

  c->pc = target;
}

/**
 * @brief Accumulator rotate family: RLCA (0x07), RRCA (0x0F), RLA
 *        (0x17), RRA (0x1F).
 *
 * Operation selector is opcode bits 4:3:
 *   00 RLCA: circular left  -- bit 7 -> C and bit 0
 *   01 RRCA: circular right -- bit 0 -> C and bit 7
 *   10 RLA : through-carry left  -- C -> bit 0, bit 7 -> C
 *   11 RRA : through-carry right -- C -> bit 7, bit 0 -> C
 *
 * Flag rules (the trap):
 *   Z: ALWAYS 0. Even when the result is zero. This is the single
 *      most-failed corner of this family. The CB-prefix counterparts
 *      RLC/RRC/RL/RR set Z = (result == 0); the accumulator versions
 *      do NOT. Different rule, same bit positions.
 *   N: 0
 *   H: 0
 *   C: bit shifted out (bit 7 for left, bit 0 for right).
 *
 * Cycles: 1 M-cycle (opcode fetch only, paid by cpu_step).
 */
void op_rotate_a(CPU* c, uint8_t opcode) {
  int op = (opcode >> 3) & 0x03;
  uint8_t a = c->r.a;
  uint8_t carry_in = (c->r.f & FLAG_C) ? 1 : 0;
  uint8_t result;
  bool new_carry;

  switch (op) {
    case 0: // RLCA
      new_carry = (a & 0x80) != 0;
      result = (uint8_t)((a << 1) | (a >> 7));
      break;
    case 1: // RRCA
      new_carry = (a & 0x01) != 0;
      result = (uint8_t)((a >> 1) | (a << 7));
      break;
    case 2: // RLA
      new_carry = (a & 0x80) != 0;
      result = (uint8_t)((a << 1) | carry_in);
      break;
    case 3: // RRA
      new_carry = (a & 0x01) != 0;
      result = (uint8_t)((a >> 1) | (carry_in << 7));
      break;
    default:
      result = 0; new_carry = false; // unreachable
  }

  c->r.a = result;
  set_flag(c, FLAG_Z, false);     // ALWAYS 0 -- not computed from result
  set_flag(c, FLAG_N, false);
  set_flag(c, FLAG_H, false);
  set_flag(c, FLAG_C, new_carry);
}

/**
 * @brief DAA (0x27). Decimal Adjust Accumulator.
 *
 * Adjusts A so that the result of a previous ADD/ADC/SUB/SBC on A
 * (treating operands as packed BCD) is itself a valid packed BCD
 * value. The N flag tells DAA whether the previous op was an
 * addition or subtraction; H and C carry information about half-
 * and full-byte BCD overflow.
 *
 * Algorithm (after ADD, N == 0):
 *   if C is set or A > 0x99:    add 0x60 to A, set C.
 *   if H is set or A_low > 0x9: add 0x06 to A.
 * After SUB (N == 1):
 *   if C is set:                subtract 0x60 from A. (C remains set.)
 *   if H is set:                subtract 0x06 from A.
 *
 * The two adjustments inside each branch combine via OR -- 0x60
 * does not touch the low nibble, so the low-nibble test gives the
 * same answer before or after the high-nibble adjustment.
 *
 * Flags:
 *   Z: result == 0 (computed from the post-adjust A).
 *   N: preserved.
 *   H: forced to 0, always.
 *   C: in ADD path, set to 1 if the high adjustment fired; otherwise 0.
 *      In SUB path, preserved (only the "if C is set" branch touches it
 *      and only writes 1, never 0).
 *
 * Cycles: 1 M-cycle.
 */
void op_daa(CPU* c, uint8_t opcode) {
  (void)opcode;

  uint8_t a = c->r.a;
  bool n  = (c->r.f & FLAG_N) != 0;
  bool h  = (c->r.f & FLAG_H) != 0;
  bool cf = (c->r.f & FLAG_C) != 0;

  uint8_t adjust = 0;
  bool new_carry = false;

  if (!n) {
    // Post-addition adjustment.
    if (cf || a > 0x99) {
      adjust |= 0x60;
      new_carry = true;
    }
    if (h || (a & 0x0F) > 0x09) {
      adjust |= 0x06;
    }
    a = (uint8_t)(a + adjust);
  } else {
    // Post-subtraction adjustment.
    if (cf) {
      adjust |= 0x60;
      new_carry = true;  // preserve the borrow
    }
    if (h) {
      adjust |= 0x06;
    }
    a = (uint8_t)(a - adjust);
  }

  c->r.a = a;
  set_flag(c, FLAG_Z, a == 0);
  // N is preserved -- do not touch it.
  set_flag(c, FLAG_H, false);
  set_flag(c, FLAG_C, new_carry);
}

/**
 * @brief CPL (0x2F). A = ~A (one's complement).
 *
 * Flags: Z and C preserved. N and H set to 1. Cycles: 1 M-cycle.
 */
void op_cpl(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.a = (uint8_t)~c->r.a;
  set_flag(c, FLAG_N, true);
  set_flag(c, FLAG_H, true);
  // Z and C preserved.
}

/**
 * @brief SCF (0x37). Set Carry Flag.
 *
 * Flags: Z preserved. N and H cleared. C set to 1. Cycles: 1 M-cycle.
 */
void op_scf(CPU* c, uint8_t opcode) {
  (void)opcode;
  set_flag(c, FLAG_N, false);
  set_flag(c, FLAG_H, false);
  set_flag(c, FLAG_C, true);
  // Z preserved.
}

/**
 * @brief CCF (0x3F). Complement Carry Flag.
 *
 * Flags: Z preserved. N and H cleared. C is toggled. Cycles: 1 M-cycle.
 */
void op_ccf(CPU* c, uint8_t opcode) {
  (void)opcode;
  bool old_c = (c->r.f & FLAG_C) != 0;
  set_flag(c, FLAG_N, false);
  set_flag(c, FLAG_H, false);
  set_flag(c, FLAG_C, !old_c);
  // Z preserved.
}

/**
 * @brief DI (0xF3). Disable interrupts immediately.
 *
 * Sets IME to DISABLED with no delay. If a previous EI had set IME
 * to ENABLING, that pending enable is cancelled (cpu_execute_instruction's
 * post-instruction check only promotes when IME is still ENABLING
 * at the end of the instruction; DI overwrites it to DISABLED).
 *
 * Cycles: 1 M-cycle. Flags: unaffected.
 */
void op_di(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->ime = IME_DISABLED;
}

/**
 * @brief EI (0xFB). Enable interrupts after a one-instruction delay.
 *
 * Sets IME to ENABLING. The actual transition to ENABLED happens at
 * the end of cpu_execute_instruction for the *following* instruction
 * (not this one), giving EI its documented one-instruction delay
 * before interrupts can fire.
 *
 * Cycles: 1 M-cycle. Flags: unaffected.
 */
void op_ei(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->ime = IME_ENABLING;
}

/**
 * @brief HALT (0x76). Suspend CPU until an interrupt is pending.
 *
 * Two paths, controlled by IME and the IE/IF state at HALT entry:
 *
 *   Normal halt: set halted=true. cpu_step's halted handling takes
 *   over from there -- it ticks the bus until (IE & IF & 0x1F) != 0,
 *   at which point it clears halted. If IME is enabled at that
 *   point, the interrupt is also serviced; otherwise execution
 *   simply resumes at the instruction after HALT.
 *
 *   HALT bug: when IME is NOT enabled (DISABLED or ENABLING) AND
 *   an interrupt is already pending at HALT entry, the CPU does
 *   not actually halt. Instead, halt_bug_pending is set so that
 *   the NEXT opcode fetch fails to increment PC. The byte at PC
 *   ends up being consumed twice. See decision 014.
 *
 * Cycles: 1 M-cycle (the opcode fetch). Subsequent halted ticks are
 * accounted for by cpu_step's halted handling, not here.
 *
 * Flags: unaffected.
 */
void op_halt(CPU* c, uint8_t opcode) {
  (void)opcode;

  uint8_t ie     = bus_peek(c->bus, 0xFFFF);
  uint8_t if_reg = bus_peek(c->bus, 0xFF0F);
  bool    pending = (ie & if_reg & 0x1F) != 0;

  if (c->ime != IME_ENABLED && pending) {
    // HALT bug: don't actually halt; arm the next-fetch quirk.
    c->halt_bug_pending = true;
  } else {
    c->halted = true;
  }
}

/**
 * @brief STOP (0x10). Single-byte stub.
 *
 * STOP's encoding is famously contested. Pan Docs and most older
 * references describe STOP as a two-byte instruction with a
 * conventionally-0x00 padding byte; this is observable on real DMG
 * hardware as the second byte being skipped on resume. SingleStepTests,
 * however, treats STOP as a one-byte instruction at the SM83 core
 * level: it expects PC to advance only past the 0x10 opcode itself,
 * not past a padding byte. The "skip" of the next byte on real
 * hardware is, on this reading, a side effect of the halt-trigger
 * mechanism rather than part of the instruction's encoding.
 *
 * We follow the SingleStepTests convention so the suite passes.
 * This means PC advances by 1 here, not 2. If we ever hook STOP up
 * to real low-power / joypad-wake behavior, the second-byte skip
 * will need to live in that wake path, not in this opcode.
 *
 * Real DMG hardware additionally uses STOP to enter low-power mode
 * until any joypad input occurs, with associated LCD/timer side
 * effects. We do not model any of that.
 *
 * Cycles: 1 M-cycle (just the opcode fetch). Flags: unaffected.
 */
void op_stop(CPU* c, uint8_t opcode) {
  (void)c;
  (void)opcode;
}

// =====================================================================
// Memory loads and stores
// =====================================================================

// --- Indirect loads to/from A through register pairs ---

/** @brief LD (BC), A (0x02). 2 M-cycles. */
void op_ld_mbc_a(CPU* c, uint8_t opcode) {
  (void)opcode;
  bus_write(c->bus, reg_get_bc(&c->r), c->r.a);
}

/** @brief LD (DE), A (0x12). 2 M-cycles. */
void op_ld_mde_a(CPU* c, uint8_t opcode) {
  (void)opcode;
  bus_write(c->bus, reg_get_de(&c->r), c->r.a);
}

/** @brief LD A, (BC) (0x0A). 2 M-cycles. */
void op_ld_a_mbc(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.a = bus_read(c->bus, reg_get_bc(&c->r));
}

/** @brief LD A, (DE) (0x1A). 2 M-cycles. */
void op_ld_a_mde(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.a = bus_read(c->bus, reg_get_de(&c->r));
}

/**
 * @brief LD (HL+), A (0x22). 2 M-cycles.
 *
 * Stores A at (HL), then increments HL. The increment happens AFTER
 * the memory access; if HL=0xFFFF, the byte is written to 0xFFFF
 * (mirrored in IE) and HL wraps to 0x0000.
 *
 * Flags: unaffected.
 */
void op_ld_mhli_a(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint16_t hl = reg_get_hl(&c->r);
  bus_write(c->bus, hl, c->r.a);
  reg_set_hl(&c->r, (uint16_t)(hl + 1));
}

/** @brief LD (HL-), A (0x32). 2 M-cycles. Stores A at (HL), then HL--. */
void op_ld_mhld_a(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint16_t hl = reg_get_hl(&c->r);
  bus_write(c->bus, hl, c->r.a);
  reg_set_hl(&c->r, (uint16_t)(hl - 1));
}

/** @brief LD A, (HL+) (0x2A). 2 M-cycles. Loads A from (HL), then HL++. */
void op_ld_a_mhli(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint16_t hl = reg_get_hl(&c->r);
  c->r.a = bus_read(c->bus, hl);
  reg_set_hl(&c->r, (uint16_t)(hl + 1));
}

/** @brief LD A, (HL-) (0x3A). 2 M-cycles. Loads A from (HL), then HL--. */
void op_ld_a_mhld(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint16_t hl = reg_get_hl(&c->r);
  c->r.a = bus_read(c->bus, hl);
  reg_set_hl(&c->r, (uint16_t)(hl - 1));
}

// --- High-page (0xFF00) accesses: LDH ---
//
// "LDH" instructions take a high-page address: the effective address
// is 0xFF00 | offset, where offset is either an immediate byte (LDH
// (n),A and LDH A,(n)) or register C (LDH (C),A and LDH A,(C)). This
// is how DMG software efficiently accesses I/O registers (0xFF00-FF7F)
// and HRAM (0xFF80-FFFE) without spending a full 16-bit address.

/** @brief LDH (n), A (0xE0). 3 M-cycles. Stores A at 0xFF00 | n. */
void op_ldh_n_a(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint8_t n = fetch8(c);
  bus_write(c->bus, (uint16_t)(0xFF00 | n), c->r.a);
}

/** @brief LDH A, (n) (0xF0). 3 M-cycles. Loads A from 0xFF00 | n. */
void op_ldh_a_n(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint8_t n = fetch8(c);
  c->r.a = bus_read(c->bus, (uint16_t)(0xFF00 | n));
}

/** @brief LDH (C), A (0xE2). 2 M-cycles. Stores A at 0xFF00 | C. */
void op_ldh_mc_a(CPU* c, uint8_t opcode) {
  (void)opcode;
  bus_write(c->bus, (uint16_t)(0xFF00 | c->r.c), c->r.a);
}

/** @brief LDH A, (C) (0xF2). 2 M-cycles. Loads A from 0xFF00 | C. */
void op_ldh_a_mc(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->r.a = bus_read(c->bus, (uint16_t)(0xFF00 | c->r.c));
}

// --- Absolute 16-bit address loads to/from A ---

/** @brief LD (nn), A (0xEA). 4 M-cycles. */
void op_ld_mnn_a(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint16_t addr = fetch16(c);
  bus_write(c->bus, addr, c->r.a);
}

/** @brief LD A, (nn) (0xFA). 4 M-cycles. */
void op_ld_a_mnn(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint16_t addr = fetch16(c);
  c->r.a = bus_read(c->bus, addr);
}

/**
 * @brief LD (nn), SP (0x08). 5 M-cycles.
 *
 * Writes SP at the absolute address nn: low byte at nn, high byte
 * at nn+1 (little-endian). nn+1 wraps at 16 bits.
 *
 * Flags: unaffected.
 */
void op_ld_mnn_sp(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint16_t addr = fetch16(c);
  bus_write(c->bus, addr,                       (uint8_t)(c->sp & 0xFF));
  bus_write(c->bus, (uint16_t)(addr + 1),       (uint8_t)(c->sp >> 8));
}

// --- SP/HL specials ---

/**
 * @brief LD SP, HL (0xF9). 2 M-cycles.
 *
 * SP = HL. The extra cycle beyond the opcode fetch is a 16-bit
 * register-to-register transfer internal cycle (no memory access).
 *
 * Flags: unaffected.
 */
void op_ld_sp_hl(CPU* c, uint8_t opcode) {
  (void)opcode;
  c->sp = reg_get_hl(&c->r);
  sys_tick(c->bus, 1);  // internal cycle
}

/**
 * @brief Compute SP + signed-8-bit-immediate, and the flags it sets.
 *
 * Used by both LD HL, SP+e (0xF8) and ADD SP, e (0xE8). They share
 * the same arithmetic and the same flag rules; only the destination
 * (HL vs SP) and the cycle count differ.
 *
 * @param sp     Current SP value.
 * @param e      Signed 8-bit offset (passed as int8_t but expanded
 *               to 16 bits for the addition).
 * @param[out] new_f  Flag byte to write (other than the preserved bits).
 *
 * Returns the resulting 16-bit value.
 *
 * Flag rule (the trap):
 *   Z: 0 always.    N: 0 always.
 *   H: carry from bit 3 of (SP_low + e_low).
 *   C: carry from bit 7 of (SP_low + e_low).
 * That is, H and C are computed as if this were an 8-bit add of
 * SP's LOW BYTE and the unsigned 8-bit value of e. Even though SP
 * is 16 bits and e is signed, the flags act like 8-bit ADD A,n.
 * This is different from every other 16-bit add.
 */
static uint16_t sp_add_e(uint16_t sp, int8_t e, uint8_t* new_f) {
  uint8_t  e_u   = (uint8_t)e;
  uint8_t  sp_lo = (uint8_t)(sp & 0xFF);
  uint16_t result = (uint16_t)(sp + (int16_t)e);

  uint8_t f = 0;
  // Z and N are 0 (no flag bits set for those positions).
  if (((sp_lo & 0x0F) + (e_u & 0x0F)) > 0x0F) f |= FLAG_H;
  if (((unsigned)sp_lo + (unsigned)e_u)  > 0xFF) f |= FLAG_C;
  *new_f = f;

  return result;
}

/**
 * @brief LD HL, SP+e (0xF8). 3 M-cycles.
 *
 * HL = SP + (signed)e. Flag rules as in sp_add_e: Z=N=0, H and C
 * are computed from the LOW-BYTE addition (8-bit ADD semantics).
 *
 * Cycles: 3 M-cycles -- opcode fetch + immediate fetch + 1 internal
 * cycle for the 16-bit ALU.
 */
void op_ld_hl_sp_e(CPU* c, uint8_t opcode) {
  (void)opcode;
  int8_t e = (int8_t)fetch8(c);
  uint8_t f;
  uint16_t result = sp_add_e(c->sp, e, &f);
  reg_set_hl(&c->r, result);
  c->r.f = f;
  sys_tick(c->bus, 1);  // internal cycle
}

/**
 * @brief ADD SP, e (0xE8). 4 M-cycles.
 *
 * SP = SP + (signed)e. Same flag rule as LD HL, SP+e.
 *
 * Cycles: 4 M-cycles -- opcode fetch + immediate fetch + 2 internal
 * cycles. The extra internal cycle compared to LD HL, SP+e
 * corresponds to the 16-bit write back to SP itself.
 */
void op_add_sp_e(CPU* c, uint8_t opcode) {
  (void)opcode;
  int8_t e = (int8_t)fetch8(c);
  uint8_t f;
  uint16_t result = sp_add_e(c->sp, e, &f);
  c->sp = result;
  c->r.f = f;
  sys_tick(c->bus, 2);  // 2 internal cycles
}

// =====================================================================
// CB-prefix instructions
// =====================================================================

/**
 * @brief CB-prefix rotates and shifts (CB 0x00 - CB 0x3F).
 *
 * Decode:
 *   bits 5:3 select the operation:
 *     0 RLC  -- rotate left circular  (bit 7 -> C and bit 0)
 *     1 RRC  -- rotate right circular (bit 0 -> C and bit 7)
 *     2 RL   -- rotate left  through carry (C -> bit 0, bit 7 -> C)
 *     3 RR   -- rotate right through carry (C -> bit 7, bit 0 -> C)
 *     4 SLA  -- shift left arithmetic (bit 0 <- 0, bit 7 -> C)
 *     5 SRA  -- shift right arithmetic (bit 7 PRESERVED, bit 0 -> C)
 *     6 SWAP -- swap high and low nibbles, C = 0
 *     7 SRL  -- shift right logical (bit 7 <- 0, bit 0 -> C)
 *   bits 2:0 select the destination register (the standard 3-bit
 *   encoding; index 6 = (HL), which adds 2 M-cycles for read+write).
 *
 * Flag rules (apply to all 8 operations):
 *   Z = (result == 0) -- COMPUTED from the result, unlike the
 *                        accumulator rotates RLCA/RRCA/RLA/RRA which
 *                        force Z to 0. Different rule, easy to get
 *                        backwards.
 *   N = 0 always.
 *   H = 0 always.
 *   C = the bit shifted/rotated out (varies by operation; SWAP forces
 *       C to 0).
 *
 * Cycles: 2 M-cycles for register destinations (CB prefix + opcode
 * fetch). 4 M-cycles when destination is (HL): CB prefix + opcode
 * fetch + memory read + memory write, the latter two paid by
 * read_r8/write_r8.
 *
 * Two operations to call out specifically:
 *   - SRA preserves bit 7 (sign extension): 0x80 SRA -> 0xC0, not
 *     0x40. SRL is the variant that zeros bit 7.
 *   - SWAP forces C to 0 regardless of input. The other 7 operations
 *     compute C from a shifted-out bit.
 */
void op_cb_rotate_shift(CPU* c, uint8_t opcode) {
  int op  = (opcode >> 3) & 0x07;
  int dst = opcode & 0x07;

  uint8_t v = read_r8(c, dst);
  uint8_t carry_in = (c->r.f & FLAG_C) ? 1 : 0;
  uint8_t result;
  bool new_carry;

  switch (op) {
    case 0: // RLC
      new_carry = (v & 0x80) != 0;
      result = (uint8_t)((v << 1) | (v >> 7));
      break;
    case 1: // RRC
      new_carry = (v & 0x01) != 0;
      result = (uint8_t)((v >> 1) | (v << 7));
      break;
    case 2: // RL
      new_carry = (v & 0x80) != 0;
      result = (uint8_t)((v << 1) | carry_in);
      break;
    case 3: // RR
      new_carry = (v & 0x01) != 0;
      result = (uint8_t)((v >> 1) | (carry_in << 7));
      break;
    case 4: // SLA
      new_carry = (v & 0x80) != 0;
      result = (uint8_t)(v << 1);
      break;
    case 5: // SRA -- sign-extending right shift; bit 7 PRESERVED
      new_carry = (v & 0x01) != 0;
      result = (uint8_t)((v >> 1) | (v & 0x80));
      break;
    case 6: // SWAP -- nibble swap; C forced to 0
      new_carry = false;
      result = (uint8_t)(((v & 0x0F) << 4) | ((v & 0xF0) >> 4));
      break;
    case 7: // SRL -- logical right shift; bit 7 ZEROED
      new_carry = (v & 0x01) != 0;
      result = (uint8_t)(v >> 1);
      break;
    default:
      result = 0; new_carry = false; // unreachable
  }

  write_r8(c, dst, result);

  set_flag(c, FLAG_Z, result == 0);   // Computed, NOT forced to 0
  set_flag(c, FLAG_N, false);
  set_flag(c, FLAG_H, false);
  set_flag(c, FLAG_C, new_carry);
}

/**
 * @brief CB BIT n, r (CB 0x40 - CB 0x7F).
 *
 * Tests bit n of the destination register and sets flags accordingly.
 * The register's value is NOT modified.
 *
 * Decode: bit number in bits 5:3, destination in bits 2:0.
 *
 * Flag rules (the trap):
 *   Z = NOT (bit n of operand). I.e., Z is 1 when the tested bit is 0.
 *   N = 0.
 *   H = 1.   <-- This is the trap. The CB rotate/shift family forces
 *                H to 0; BIT forces it to 1. Easy to copy the wrong
 *                rule across.
 *   C = preserved.
 *
 * Cycles: 2 M-cycles for register destinations. 3 M-cycles for (HL) --
 * just CB prefix + opcode + memory read. There is NO write back, so
 * BIT (HL) is one M-cycle shorter than RES/SET (HL).
 */
void op_cb_bit(CPU* c, uint8_t opcode) {
  int bit_n = (opcode >> 3) & 0x07;
  int src   = opcode & 0x07;

  uint8_t v = read_r8(c, src);
  bool bit_set = (v & (uint8_t)(1u << bit_n)) != 0;

  set_flag(c, FLAG_Z, !bit_set);
  set_flag(c, FLAG_N, false);
  set_flag(c, FLAG_H, true);   // H = 1, NOT 0
  // C preserved.
}

/**
 * @brief CB RES n, r (CB 0x80 - CB 0xBF).
 *
 * Clears bit n of the destination register.
 *
 * Decode: bit number in bits 5:3, destination in bits 2:0.
 *
 * Flag rules: NONE. All four flags preserved.
 *
 * Cycles: 2 M-cycles for register destinations. 4 M-cycles for (HL)
 * (CB + opcode + read + write).
 */
void op_cb_res(CPU* c, uint8_t opcode) {
  int bit_n = (opcode >> 3) & 0x07;
  int dst   = opcode & 0x07;

  uint8_t v = read_r8(c, dst);
  v = (uint8_t)(v & ~(uint8_t)(1u << bit_n));
  write_r8(c, dst, v);
  // No flag changes.
}

/**
 * @brief CB SET n, r (CB 0xC0 - CB 0xFF).
 *
 * Sets bit n of the destination register.
 *
 * Decode: bit number in bits 5:3, destination in bits 2:0.
 *
 * Flag rules: NONE. All four flags preserved.
 *
 * Cycles: 2 M-cycles for register destinations. 4 M-cycles for (HL)
 * (CB + opcode + read + write).
 */
void op_cb_set(CPU* c, uint8_t opcode) {
  int bit_n = (opcode >> 3) & 0x07;
  int dst   = opcode & 0x07;

  uint8_t v = read_r8(c, dst);
  v = (uint8_t)(v | (uint8_t)(1u << bit_n));
  write_r8(c, dst, v);
  // No flag changes.
}

/**
 * @brief LD (HL), n (0x36).
 *
 * Stores an 8-bit immediate into the byte at (HL). The "register
 * destination" of the LD r,n family that we missed because index 6
 * is (HL) rather than a register.
 *
 * Cycles: 3 M-cycles -- opcode fetch + immediate fetch + memory write.
 * Flags: unaffected.
 */
void op_ld_mhl_n(CPU* c, uint8_t opcode) {
  (void)opcode;
  uint8_t n = fetch8(c);
  bus_write(c->bus, reg_get_hl(&c->r), n);
}