/**
 * @file main.c
 * @brief Host entry point: load a ROM and run it.
 *
 * Usage: emu <rom.gb> [options]
 *
 *   --max-frames N             Stop after N frames (default 7200).
 *   --dump-framebuffer FILE    After running, write the PPU framebuffer
 *                              to FILE as a 160x144 P6 PPM image.
 *
 * Runs the given ROM, printing serial-port output to stdout (which
 * is how Blargg's test ROMs report results). Real-time pacing is
 * not applied -- frames are run as fast as possible.
 *
 * The PPM dump uses a fixed 4-shade grayscale palette mapping the
 * 0-3 indices the PPU produces:
 *   0 = white  (255, 255, 255)
 *   1 = light  (170, 170, 170)
 *   2 = dark   ( 85,  85,  85)
 *   3 = black  (  0,   0,   0)
 */

#include "emu.h"
#include "cart.h"
#include "ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t kGrayscalePalette[4][3] = {
  {255, 255, 255},
  {170, 170, 170},
  { 85,  85,  85},
  {  0,   0,   0},
};

static int dump_framebuffer_ppm(const uint8_t* fb, const char* path) {
  FILE* fp = fopen(path, "wb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open '%s' for writing\n", path);
    return 1;
  }
  fprintf(fp, "P6\n%d %d\n255\n", PPU_LCD_WIDTH, PPU_LCD_HEIGHT);
  for (int i = 0; i < PPU_LCD_WIDTH * PPU_LCD_HEIGHT; i++) {
    uint8_t idx = fb[i] & 0x03;
    if (fwrite(kGrayscalePalette[idx], 1, 3, fp) != 3) {
      fprintf(stderr, "Short write to '%s'\n", path);
      fclose(fp);
      return 1;
    }
  }
  fclose(fp);
  return 0;
}

int main(int argc, char** argv) {
  const char* rom_path = NULL;
  long max_frames = 7200;
  const char* dump_path = NULL;

  // Parse args. First positional = ROM path; flags follow.
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc) {
      char* end;
      max_frames = strtol(argv[++i], &end, 10);
      if (*end != '\0' || max_frames <= 0) {
        fprintf(stderr, "--max-frames requires a positive integer\n");
        return 1;
      }
    } else if (strcmp(argv[i], "--dump-framebuffer") == 0 && i + 1 < argc) {
      dump_path = argv[++i];
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return 1;
    } else if (rom_path == NULL) {
      rom_path = argv[i];
    } else {
      fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
      return 1;
    }
  }

  if (rom_path == NULL) {
    fprintf(stderr,
            "Usage: %s <rom.gb> [--max-frames N] [--dump-framebuffer FILE]\n",
            argv[0]);
    return 1;
  }

  Emu* e = emu_create();
  if (e == NULL) {
    fprintf(stderr, "emu_create failed\n");
    return 1;
  }

  int rc = emu_load_rom(e, rom_path);
  if (rc != 0) {
    fprintf(stderr, "Failed to load ROM '%s' (cart error %d)\n", rom_path, rc);
    emu_destroy(e);
    return 1;
  }

  for (long i = 0; i < max_frames; i++) {
    emu_run_frame(e);
  }

  fputc('\n', stdout);

  if (dump_path != NULL) {
    if (dump_framebuffer_ppm(emu_framebuffer(e), dump_path) != 0) {
      emu_destroy(e);
      return 1;
    }
    fprintf(stderr, "Dumped framebuffer to %s\n", dump_path);
  }

  emu_destroy(e);
  return 0;
}