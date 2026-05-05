/**
 * @file test_halt_bug.c
 * @brief Standalone test for the HALT bug (decision 014).
 *
 * The HALT bug is observable only across TWO instructions. SingleStepTests
 * runs one instruction per case, so this test exists outside that harness.
 *
 * Scenario: HALT at 0x100 with IME=DISABLED and a pending interrupt.
 * The next instruction is `LD A, n` (0x3E imm) at 0x101..0x102, with
 * imm = 0x42. Without the bug, A would become 0x42. With the bug, the
 * post-HALT opcode fetch reads 0x3E without advancing PC, so the next
 * fetch (LD A, n's immediate fetch) reads at PC=0x101 again, picking
 * up 0x3E (the LD A, n opcode itself) as the immediate. So A becomes
 * 0x3E, not 0x42.
 *
 * We also run a control case (HALT but with no pending interrupt) to
 * confirm it sets halted=true normally.
 */

#include "cpu.h"
#include "cpu_internal.h"
#include "bus.h"
#include "test_bus.h"

#include <stdio.h>
#include <stdlib.h>

static int run_halt_bug_case(void) {
  Bus* b = bus_create(NULL);
  CPU* c = cpu_create(b);

  // Set up: HALT at 0x100, LD A,n at 0x101 with immediate 0x42.
  test_bus_poke_raw(b, 0x0100, 0x76);  // HALT
  test_bus_poke_raw(b, 0x0101, 0x3E);  // LD A, n
  test_bus_poke_raw(b, 0x0102, 0x42);  // immediate

  // Trigger bug: IME disabled, IE & IF have overlapping bits.
  test_bus_poke_raw(b, 0xFFFF, 0x01);  // IE: VBlank enabled
  test_bus_poke_raw(b, 0xFF0F, 0x01);  // IF: VBlank pending

  c->pc    = 0x0100;
  c->ime   = IME_DISABLED;
  c->r.a   = 0x00;
  c->halted = false;
  c->halt_bug_pending = false;

  // Run HALT.
  cpu_execute_instruction(c);
  // Expectations after HALT:
  //   PC = 0x0101 (HALT consumed one byte)
  //   halted = false (bug took precedence over halting)
  //   halt_bug_pending = true
  if (c->pc != 0x0101) {
    fprintf(stderr, "  HALT step: PC = 0x%04X, expected 0x0101\n", c->pc);
    goto fail;
  }
  if (c->halted) {
    fprintf(stderr, "  HALT step: halted=true; expected false (bug case)\n");
    goto fail;
  }
  if (!c->halt_bug_pending) {
    fprintf(stderr, "  HALT step: halt_bug_pending=false; expected true\n");
    goto fail;
  }

  // Run the next instruction (the LD A, n at 0x101).
  cpu_execute_instruction(c);
  // With the bug: the opcode fetch reads 0x3E at 0x101 and PC stays at 0x101.
  // Then LD A,n's fetch8 reads at 0x101 again (still 0x3E) and advances PC to 0x102.
  // So A = 0x3E (the opcode byte), NOT 0x42 (the immediate).
  if (c->r.a != 0x3E) {
    fprintf(stderr, "  Bug case: A = 0x%02X, expected 0x3E (the LD A, n opcode itself)\n",
            c->r.a);
    goto fail;
  }
  if (c->pc != 0x0102) {
    fprintf(stderr, "  Bug case: PC = 0x%04X, expected 0x0102\n", c->pc);
    goto fail;
  }
  if (c->halt_bug_pending) {
    fprintf(stderr, "  Bug case: halt_bug_pending still true; expected cleared\n");
    goto fail;
  }

  cpu_destroy(c);
  bus_destroy(b);
  return 0;

fail:
  cpu_destroy(c);
  bus_destroy(b);
  return 1;
}

static int run_normal_halt_case(void) {
  // Control: HALT with no pending interrupt -> halted=true, no bug.
  Bus* b = bus_create(NULL);
  CPU* c = cpu_create(b);

  test_bus_poke_raw(b, 0x0100, 0x76);  // HALT
  test_bus_poke_raw(b, 0xFFFF, 0x00);  // IE: nothing
  test_bus_poke_raw(b, 0xFF0F, 0x00);  // IF: nothing

  c->pc    = 0x0100;
  c->ime   = IME_DISABLED;
  c->halted = false;
  c->halt_bug_pending = false;

  cpu_execute_instruction(c);

  if (c->pc != 0x0101) {
    fprintf(stderr, "  Normal HALT: PC = 0x%04X, expected 0x0101\n", c->pc);
    goto fail;
  }
  if (!c->halted) {
    fprintf(stderr, "  Normal HALT: halted=false; expected true\n");
    goto fail;
  }
  if (c->halt_bug_pending) {
    fprintf(stderr, "  Normal HALT: halt_bug_pending=true; expected false\n");
    goto fail;
  }

  cpu_destroy(c);
  bus_destroy(b);
  return 0;

fail:
  cpu_destroy(c);
  bus_destroy(b);
  return 1;
}

int main(void) {
  int fails = 0;
  printf("HALT bug case (IME=0, pending interrupt): ");
  if (run_halt_bug_case() == 0) printf("PASS\n");
  else { printf("FAIL\n"); fails++; }

  printf("Normal HALT (no pending interrupt):       ");
  if (run_normal_halt_case() == 0) printf("PASS\n");
  else { printf("FAIL\n"); fails++; }

  return fails ? 1 : 0;
}
