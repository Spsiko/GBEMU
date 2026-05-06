/**
 * @file test_emu_smoke.c
 * @brief End-to-end smoke test: hand-built ROM, real emu loop, real
 *        serial. Confirms CPU + bus + timer + serial + emu integrate
 *        correctly without needing an external test ROM.
 *
 * The ROM:
 *   - Lives at 0x0150 (just past the cart header).
 *   - Writes 'O' to SB (0xFF01), then 0x81 to SC (0xFF02), printing 'O'.
 *   - Writes 'K' to SB, then 0x81 to SC, printing 'K'.
 *   - Writes '\n' to SB, then 0x81 to SC, printing newline.
 *   - JR -2 to itself (infinite loop).
 *
 * The cart header (0x0100-0x014F) needs a valid Nintendo logo at
 * 0x0104-0x0133 and a header checksum at 0x014D for cart_load to
 * accept it. We synthesize both.
 */

#include "../../include/emu.h"
#include "../../include/cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Nintendo logo bytes that the boot ROM (and our checksum routine)
// expects at 0x0104-0x0133. cart_load checks this.
static const uint8_t kNintendoLogo[48] = {
  0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,0x00,0x0C,0x00,0x0D,
  0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,
  0xBB,0xBB,0x67,0x63,0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E,
};

int main(void) {
  uint8_t rom[0x8000] = {0};

  // Entry point at 0x0100: jump to 0x0150.
  rom[0x0100] = 0x00;       // NOP
  rom[0x0101] = 0xC3;       // JP 0x0150
  rom[0x0102] = 0x50;
  rom[0x0103] = 0x01;

  // Nintendo logo at 0x0104.
  memcpy(&rom[0x0104], kNintendoLogo, 48);

  // Title and other header bytes can stay zero (cart_load tolerates that).

  // Header checksum at 0x014D. Algorithm: x = 0; for addr 0x134..0x14C: x = x - rom[addr] - 1.
  uint8_t checksum = 0;
  for (uint16_t a = 0x0134; a <= 0x014C; a++) {
    checksum = (uint8_t)(checksum - rom[a] - 1);
  }
  rom[0x014D] = checksum;

  // Program at 0x0150.
  uint16_t pc = 0x0150;
  const char* msg = "OK\n";
  for (const char* p = msg; *p; p++) {
    // LD A, *p
    rom[pc++] = 0x3E;
    rom[pc++] = (uint8_t)*p;
    // LDH (0x01), A   -- write SB
    rom[pc++] = 0xE0;
    rom[pc++] = 0x01;
    // LD A, 0x81
    rom[pc++] = 0x3E;
    rom[pc++] = 0x81;
    // LDH (0x02), A   -- write SC, triggers print
    rom[pc++] = 0xE0;
    rom[pc++] = 0x02;
  }
  // JR -2  (infinite loop on this 2-byte instruction)
  rom[pc++] = 0x18;
  rom[pc++] = 0xFE;

  // Write to a temp file (cart_load takes a path).
  char path[] = "/tmp/smoke_rom_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) { perror("mkstemp"); return 1; }
  if (write(fd, rom, sizeof(rom)) != (ssize_t)sizeof(rom)) {
    perror("write"); close(fd); unlink(path); return 1;
  }
  close(fd);

  // Capture stdout to a pipe so we can check what serial wrote.
  fflush(stdout);
  int saved_stdout = dup(1);
  int pipefd[2];
  if (pipe(pipefd) != 0) { perror("pipe"); return 1; }
  dup2(pipefd[1], 1);
  close(pipefd[1]);

  Emu* e = emu_create();
  int rc = emu_load_rom(e, path);
  if (rc != 0) {
    dup2(saved_stdout, 1);
    fprintf(stderr, "cart_load failed: %d\n", rc);
    emu_destroy(e); unlink(path); return 1;
  }

  // Run a few frames -- 3 chars at ~5 M-cycles each is far less than
  // one frame, but we run a couple anyway to be safe.
  for (int i = 0; i < 3; i++) emu_run_frame(e);

  // Restore stdout, read the pipe.
  fflush(stdout);
  dup2(saved_stdout, 1);
  close(saved_stdout);

  char captured[64] = {0};
  ssize_t n = read(pipefd[0], captured, sizeof(captured) - 1);
  close(pipefd[0]);
  if (n < 0) { perror("read"); emu_destroy(e); unlink(path); return 1; }

  emu_destroy(e);
  unlink(path);

  printf("Captured (%zd bytes): \"", n);
  for (ssize_t i = 0; i < n; i++) {
    char c = captured[i];
    if (c == '\n') printf("\\n");
    else if (c >= 32 && c < 127) putchar(c);
    else printf("\\x%02X", (unsigned char)c);
  }
  printf("\"\n");

  if (n == 3 && memcmp(captured, "OK\n", 3) == 0) {
    printf("PASS: emu+cart+bus+cpu+serial smoke test\n");
    return 0;
  }
  printf("FAIL: expected \"OK\\n\", got %zd bytes\n", n);
  return 1;
}