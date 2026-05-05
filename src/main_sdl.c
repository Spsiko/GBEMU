/**
 * @file main_sdl.c
 * @brief SDL2 host: window + keyboard + real-time pacing.
 *
 * Usage: emu_sdl <rom.gb>
 *
 * Keys:
 *   Arrow keys     D-pad
 *   Z              A
 *   X              B
 *   Enter          Start
 *   Backspace      Select
 *   Esc            Quit
 *
 * Per decision 019, frame pacing is the host's responsibility. We
 * track an absolute target tick for each frame's presentation using
 * SDL_GetPerformanceCounter, sleep coarsely with SDL_Delay when
 * there is plenty of slack, and spin for the last fraction of a ms.
 * This avoids the drift that "sleep N us per frame" would accumulate,
 * and works correctly on any display refresh rate.
 */

#include "emu.h"
#include "joypad.h"
#include "ppu.h"

#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Same 4-shade palette as main.c. Could be lifted to a shared header
// if a third host appears; not worth it for two.
static const uint8_t kGrayscalePalette[4][3] = {
  {255, 255, 255},
  {170, 170, 170},
  { 85,  85,  85},
  {  0,   0,   0},
};

// 70224 T-cycles / 4194304 Hz = 16742.04 us per frame.
static const uint64_t kFrameMicros = 16742;

// Default initial scale: 4x = 640 x 576 window.
static const int kInitialScale = 4;

// Convert the PPU's 0-3 index framebuffer to packed RGB24.
static void framebuffer_to_rgb(const uint8_t* fb, uint8_t* out) {
  for (int i = 0; i < PPU_LCD_WIDTH * PPU_LCD_HEIGHT; i++) {
    uint8_t idx = fb[i] & 0x03;
    out[3 * i + 0] = kGrayscalePalette[idx][0];
    out[3 * i + 1] = kGrayscalePalette[idx][1];
    out[3 * i + 2] = kGrayscalePalette[idx][2];
  }
}

// Build the joypad bitmask from the current keyboard state.
// Idempotent (per decision 029) -- safe to call every frame even when
// nothing has changed.
static uint8_t poll_buttons(const uint8_t* keys) {
  uint8_t mask = 0;
  if (keys[SDL_SCANCODE_RIGHT])     mask |= (1u << JOYPAD_RIGHT);
  if (keys[SDL_SCANCODE_LEFT])      mask |= (1u << JOYPAD_LEFT);
  if (keys[SDL_SCANCODE_UP])        mask |= (1u << JOYPAD_UP);
  if (keys[SDL_SCANCODE_DOWN])      mask |= (1u << JOYPAD_DOWN);
  if (keys[SDL_SCANCODE_Z])         mask |= (1u << JOYPAD_A);
  if (keys[SDL_SCANCODE_X])         mask |= (1u << JOYPAD_B);
  if (keys[SDL_SCANCODE_RETURN])    mask |= (1u << JOYPAD_START);
  if (keys[SDL_SCANCODE_BACKSPACE]) mask |= (1u << JOYPAD_SELECT);
  return mask;
}

// Wait until an absolute target tick. Yields to the OS in 1ms chunks
// while there is significant slack, spins through the last fraction.
// SDL_Delay's actual resolution on Windows is up to ~16ms, so we only
// trust it for coarse waits and spin the rest.
static void wait_until(uint64_t target, uint64_t perf_freq) {
  for (;;) {
    uint64_t now = SDL_GetPerformanceCounter();
    if (now >= target) return;
    uint64_t remaining_us = ((target - now) * 1000000) / perf_freq;
    if (remaining_us > 3000) {
      SDL_Delay(1);
    }
    // else spin; loop iteration is cheap.
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <rom.gb>\n", argv[0]);
    return 1;
  }
  const char* rom_path = argv[1];

  // Build emulator before SDL so a bad ROM fails fast without
  // bringing up a window.
  Emu* e = emu_create();
  if (e == NULL) {
    fprintf(stderr, "emu_create failed\n");
    return 1;
  }
  int rc = emu_load_rom(e, rom_path);
  if (rc != 0) {
    fprintf(stderr, "Failed to load ROM '%s' (cart error %d)\n",
            rom_path, rc);
    emu_destroy(e);
    return 1;
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    emu_destroy(e);
    return 1;
  }

  // Nearest-neighbor scaling: sharp pixels, not blur.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

  SDL_Window* window = SDL_CreateWindow(
      "DMG Emulator",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      PPU_LCD_WIDTH * kInitialScale, PPU_LCD_HEIGHT * kInitialScale,
      SDL_WINDOW_RESIZABLE);
  if (window == NULL) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    emu_destroy(e);
    return 1;
  }

  SDL_Renderer* renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED);
  if (renderer == NULL) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    emu_destroy(e);
    return 1;
  }

  // Logical size pins the rendered region to 160x144; SDL upscales to
  // the window's actual pixel size with letterboxing on aspect mismatch.
  SDL_RenderSetLogicalSize(renderer, PPU_LCD_WIDTH, PPU_LCD_HEIGHT);

  SDL_Texture* texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
      PPU_LCD_WIDTH, PPU_LCD_HEIGHT);
  if (texture == NULL) {
    fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    emu_destroy(e);
    return 1;
  }

  static uint8_t pixels[PPU_LCD_WIDTH * PPU_LCD_HEIGHT * 3];
  static int16_t audio_samples[2048];

  SDL_AudioSpec want;
  SDL_AudioSpec have;
  SDL_zero(want);
  want.freq = 44100;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 1024;

  SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (audio_dev == 0) {
    fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    emu_destroy(e);
    return 1;
  }
  SDL_PauseAudioDevice(audio_dev, 0);

  // Pacing setup: track an absolute target for each frame's present.
  // Advancing the target by exactly frame_ticks per iteration keeps
  // the long-term frame rate locked to 59.7275 fps without drift.
  uint64_t perf_freq   = SDL_GetPerformanceFrequency();
  uint64_t frame_ticks = (perf_freq * kFrameMicros) / 1000000;
  uint64_t next_frame  = SDL_GetPerformanceCounter() + frame_ticks;

  int running = 1;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = 0;
      } else if (event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_ESCAPE) {
        running = 0;
      }
    }

    // Sample keyboard, push to joypad. Idempotent if unchanged.
    const uint8_t* keys = SDL_GetKeyboardState(NULL);
    emu_set_buttons(e, poll_buttons(keys));

    // Run one DMG frame's worth of cycles, unmetered (decision 019).
    emu_run_frame(e);

    // Queue any audio samples generated by the APU. Keep the queue short
    // so audio stays responsive rather than drifting seconds behind video.
    if (SDL_GetQueuedAudioSize(audio_dev) < 4096U * sizeof(int16_t)) {
      size_t n = emu_audio_read(e, audio_samples, 2048U);
      if (n > 0U) {
        SDL_QueueAudio(audio_dev, audio_samples, (uint32_t)(n * sizeof(int16_t)));
      }
    }

    // Push framebuffer to GPU and present.
    framebuffer_to_rgb(emu_framebuffer(e), pixels);
    SDL_UpdateTexture(texture, NULL, pixels, PPU_LCD_WIDTH * 3);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    // Pace to next frame deadline.
    wait_until(next_frame, perf_freq);
    next_frame += frame_ticks;

    // If we've fallen more than one full frame behind (debugger
    // pause, OS hiccup), resync rather than spiral catching up.
    uint64_t now = SDL_GetPerformanceCounter();
    if (now > next_frame + frame_ticks) {
      next_frame = now + frame_ticks;
    }
  }

  SDL_CloseAudioDevice(audio_dev);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  emu_destroy(e);
  return 0;
}