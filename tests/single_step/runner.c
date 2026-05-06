/**
 * @file runner.c
 * @brief SingleStepTests sm83 harness.
 *
 * Loads a JSON test file, runs each test case through cpu_step on a
 * test bus, compares final state, reports pass/fail. Prints a diff
 * for each failure showing what differed (registers, RAM, or both).
 *
 * Usage: runner <path-to-test-file.json>
 *
 * Test format (per case):
 *   {
 *     "name": "00 0000",
 *     "initial": {
 *       "pc", "sp", "a", "b", "c", "d", "e", "f", "h", "l",
 *       "ime", "ie",
 *       "ram": [[addr, value], ...]
 *     },
 *     "final": { ... same shape ... },
 *     "cycles": [ ... ]   // not consumed in the loose checker
 *   }
 *
 * Loose checking: register and RAM final state. Strict cycle/order
 * checking is intentionally not implemented; instr_timing.gb covers
 * the same ground from the system level. See decisions.md 007.
 *
 * NOTE: This harness includes cpu_internal.h to set CPU register
 * state directly. That is acceptable because the harness lives
 * inside the cpu module's test scope; production code outside the
 * cpu module must use cpu.h only.
 */

#include "cpu.h"
#include "cpu_internal.h"
#include "bus.h"
#include "test_bus.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- File loading ---

static char* read_file(const char* path) {
  FILE* fp = fopen(path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open %s\n", path);
    return NULL;
  }
  if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
  long sz = ftell(fp);
  if (sz < 0) { fclose(fp); return NULL; }
  rewind(fp);

  char* buf = malloc((size_t)sz + 1);
  if (buf == NULL) { fclose(fp); return NULL; }

  size_t n = fread(buf, 1, (size_t)sz, fp);
  fclose(fp);
  if (n != (size_t)sz) { free(buf); return NULL; }
  buf[sz] = '\0';
  return buf;
}

// --- JSON helpers ---

// Read an integer field by name. Returns 0 if missing.
static int j_int(const cJSON* obj, const char* name) {
  cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
  return (cJSON_IsNumber(item)) ? item->valueint : 0;
}

// --- State setup and verification ---

static void apply_state(CPU* c, Bus* b, const cJSON* state) {
  test_bus_reset(b);

  c->pc    = (uint16_t)j_int(state, "pc");
  c->sp    = (uint16_t)j_int(state, "sp");
  c->r.a   = (uint8_t)j_int(state, "a");
  c->r.b   = (uint8_t)j_int(state, "b");
  c->r.c   = (uint8_t)j_int(state, "c");
  c->r.d   = (uint8_t)j_int(state, "d");
  c->r.e   = (uint8_t)j_int(state, "e");
  c->r.f   = (uint8_t)j_int(state, "f");
  c->r.h   = (uint8_t)j_int(state, "h");
  c->r.l   = (uint8_t)j_int(state, "l");

  // IME may be 0 or 1; some tests omit it.
  cJSON* ime = cJSON_GetObjectItemCaseSensitive(state, "ime");
  if (cJSON_IsNumber(ime)) {
    c->ime = (ime->valueint != 0) ? IME_ENABLED : IME_DISABLED;
  } else {
    c->ime = IME_DISABLED;
  }

  c->halted = false;
  c->halt_bug_pending = false;

  // IE register is poked into the bus at 0xFFFF if present.
  cJSON* ie = cJSON_GetObjectItemCaseSensitive(state, "ie");
  if (cJSON_IsNumber(ie)) {
    test_bus_poke_raw(b, 0xFFFF, (uint8_t)ie->valueint);
  }

  // Apply RAM contents.
  cJSON* ram = cJSON_GetObjectItemCaseSensitive(state, "ram");
  if (cJSON_IsArray(ram)) {
    cJSON* pair;
    cJSON_ArrayForEach(pair, ram) {
      if (cJSON_IsArray(pair) && cJSON_GetArraySize(pair) >= 2) {
        cJSON* a = cJSON_GetArrayItem(pair, 0);
        cJSON* v = cJSON_GetArrayItem(pair, 1);
        if (cJSON_IsNumber(a) && cJSON_IsNumber(v)) {
          test_bus_poke_raw(b, (uint16_t)a->valueint, (uint8_t)v->valueint);
        }
      }
    }
  }
}

// Returns 0 on match, non-zero on mismatch. Prints diffs to stderr.
static int verify_state(const CPU* c, const Bus* b, const cJSON* expected,
                        const char* test_name) {
  int fail = 0;

  #define CHECK(field, actual, exp) do {                                   \
      int e = j_int(expected, exp);                                        \
      if ((int)(actual) != e) {                                            \
        fprintf(stderr, "  %s: %s = 0x%X, expected 0x%X\n",                \
                test_name, field, (unsigned)(actual), (unsigned)e);        \
        fail = 1;                                                          \
      }                                                                    \
  } while (0)

  CHECK("pc", c->pc, "pc");
  CHECK("sp", c->sp, "sp");
  CHECK("a",  c->r.a, "a");
  CHECK("b",  c->r.b, "b");
  CHECK("c",  c->r.c, "c");
  CHECK("d",  c->r.d, "d");
  CHECK("e",  c->r.e, "e");
  CHECK("f",  c->r.f, "f");
  CHECK("h",  c->r.h, "h");
  CHECK("l",  c->r.l, "l");

  #undef CHECK

  // Verify RAM addresses listed in the expected state.
  cJSON* ram = cJSON_GetObjectItemCaseSensitive(expected, "ram");
  if (cJSON_IsArray(ram)) {
    cJSON* pair;
    cJSON_ArrayForEach(pair, ram) {
      if (cJSON_IsArray(pair) && cJSON_GetArraySize(pair) >= 2) {
        cJSON* a = cJSON_GetArrayItem(pair, 0);
        cJSON* v = cJSON_GetArrayItem(pair, 1);
        if (cJSON_IsNumber(a) && cJSON_IsNumber(v)) {
          uint16_t addr  = (uint16_t)a->valueint;
          uint8_t  exp_v = (uint8_t)v->valueint;
          uint8_t  got_v = test_bus_peek_raw(b, addr);
          if (got_v != exp_v) {
            fprintf(stderr, "  %s: RAM[0x%04X] = 0x%02X, expected 0x%02X\n",
                    test_name, addr, got_v, exp_v);
            fail = 1;
          }
        }
      }
    }
  }

  return fail;
}

// --- Test runner ---

static int run_tests(const cJSON* tests) {
  Bus* b = bus_create(NULL);
  CPU* c = cpu_create(b);
  if (b == NULL || c == NULL) {
    fprintf(stderr, "Failed to construct CPU or Bus\n");
    return -1;
  }

  int pass = 0, fail = 0;
  cJSON* test;
  cJSON_ArrayForEach(test, tests) {
    cJSON* name_item = cJSON_GetObjectItemCaseSensitive(test, "name");
    const char* name = (cJSON_IsString(name_item)) ? name_item->valuestring : "?";

    cJSON* initial = cJSON_GetObjectItemCaseSensitive(test, "initial");
    cJSON* final_  = cJSON_GetObjectItemCaseSensitive(test, "final");
    if (initial == NULL || final_ == NULL) continue;

    apply_state(c, b, initial);
    cpu_execute_instruction(c);

    if (verify_state(c, b, final_, name) == 0) {
      pass++;
    } else {
      fail++;
    }
  }

  cpu_destroy(c);
  bus_destroy(b);

  printf("Passed: %d\nFailed: %d\n", pass, fail);
  return (fail == 0) ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <test-file.json>\n", argv[0]);
    return 2;
  }

  char* text = read_file(argv[1]);
  if (text == NULL) return 2;

  cJSON* root = cJSON_Parse(text);
  free(text);
  if (root == NULL) {
    fprintf(stderr, "JSON parse error in %s\n", argv[1]);
    return 2;
  }

  if (!cJSON_IsArray(root)) {
    fprintf(stderr, "Expected top-level JSON array in %s\n", argv[1]);
    cJSON_Delete(root);
    return 2;
  }

  int rc = run_tests(root);
  cJSON_Delete(root);
  return rc;
}