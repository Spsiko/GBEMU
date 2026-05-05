/**
 * @file apu.c
 * @brief Basic DMG APU for square channels and noise.
 *
 * Accuracy limits:
 * - Channel 1 sweep register is stored but sweep timing is not applied.
 * - Channel 3 wave is register-stubbed only.
 * - Length counters and envelopes are approximated through the 512 Hz
 *   frame sequencer.
 * - Output is mixed to mono for the SDL host.
 *
 * The goal is broader audible output without destabilizing the verified
 * CPU/PPU/timer core. Cycle-perfect audio can come later, after the rest
 * of the emulator stops finding new ways to be dramatic.
 */

#include "apu.h"

#include <stdlib.h>
#include <string.h>

#define DMG_CLOCK_HZ       4194304U
#define AUDIO_SAMPLE_RATE  44100U
#define SAMPLE_PERIOD_Q16  ((uint32_t)(((uint64_t)DMG_CLOCK_HZ << 16) / AUDIO_SAMPLE_RATE))
#define RING_CAPACITY      8192U

#define FRAME_SEQ_PERIOD_T 8192U

#define REG_NR10 0xFF10U
#define REG_NR11 0xFF11U
#define REG_NR12 0xFF12U
#define REG_NR13 0xFF13U
#define REG_NR14 0xFF14U
#define REG_NR21 0xFF16U
#define REG_NR22 0xFF17U
#define REG_NR23 0xFF18U
#define REG_NR24 0xFF19U
#define REG_NR30 0xFF1AU
#define REG_NR41 0xFF20U
#define REG_NR42 0xFF21U
#define REG_NR43 0xFF22U
#define REG_NR44 0xFF23U
#define REG_NR50 0xFF24U
#define REG_NR51 0xFF25U
#define REG_NR52 0xFF26U

/* Duty bit patterns, sampled at duty_pos 0..7. */
static const uint8_t kDuty[4][8] = {
  {0,0,0,0,0,0,0,1}, /* 12.5% */
  {1,0,0,0,0,0,0,1}, /* 25% */
  {1,0,0,0,0,1,1,1}, /* 50% */
  {0,1,1,1,1,1,1,0}, /* 75% */
};

static const uint16_t kNoiseDivisors[8] = {
  8U, 16U, 32U, 48U, 64U, 80U, 96U, 112U
};

typedef struct SquareChannel {
  uint8_t enabled;
  uint8_t dac_enabled;
  uint8_t duty;
  uint8_t duty_pos;

  uint8_t volume;
  uint8_t envelope_initial;
  uint8_t envelope_period;
  uint8_t envelope_increase;
  uint8_t envelope_counter;

  uint8_t length_enabled;
  uint16_t length_counter;

  uint16_t frequency;
  uint16_t timer;
} SquareChannel;

typedef struct NoiseChannel {
  uint8_t enabled;
  uint8_t dac_enabled;

  uint8_t volume;
  uint8_t envelope_initial;
  uint8_t envelope_period;
  uint8_t envelope_increase;
  uint8_t envelope_counter;

  uint8_t length_enabled;
  uint16_t length_counter;

  uint8_t clock_shift;
  uint8_t width_mode;
  uint8_t divisor_code;
  uint16_t timer;
  uint16_t lfsr;
} NoiseChannel;

struct Apu {
  uint8_t regs[0x30]; /* 0xFF10-0xFF3F */
  uint8_t master_enable;

  SquareChannel ch1;
  SquareChannel ch2;
  NoiseChannel ch4;

  uint32_t frame_seq_counter;
  uint8_t frame_seq_step;

  uint32_t sample_accum_q16;
  int16_t ring[RING_CAPACITY];
  size_t read_pos;
  size_t write_pos;
  size_t count;
};

static uint8_t reg_index(uint16_t addr) {
  return (uint8_t)(addr - 0xFF10U);
}

static uint16_t square_period(uint16_t frequency) {
  uint16_t period = (uint16_t)((2048U - (frequency & 0x07FFU)) * 4U);
  return (period == 0U) ? 4U : period;
}

static uint16_t noise_period(uint8_t divisor_code, uint8_t clock_shift) {
  uint16_t divisor = kNoiseDivisors[divisor_code & 0x07U];
  uint32_t period = (uint32_t)divisor << (clock_shift & 0x0FU);
  if (period == 0U) return 8U;
  if (period > 0xFFFFU) return 0xFFFFU;
  return (uint16_t)period;
}

static void square_update_dac(SquareChannel* ch, uint8_t nrx2) {
  ch->dac_enabled = (nrx2 & 0xF8U) ? 1U : 0U;
  if (!ch->dac_enabled) ch->enabled = 0U;
}

static void noise_update_dac(NoiseChannel* ch, uint8_t nr42) {
  ch->dac_enabled = (nr42 & 0xF8U) ? 1U : 0U;
  if (!ch->dac_enabled) ch->enabled = 0U;
}

static void square_load_envelope(SquareChannel* ch, uint8_t nrx2) {
  ch->envelope_initial = (uint8_t)(nrx2 >> 4);
  ch->volume = ch->envelope_initial;
  ch->envelope_increase = (nrx2 & 0x08U) ? 1U : 0U;
  ch->envelope_period = (uint8_t)(nrx2 & 0x07U);
  ch->envelope_counter = ch->envelope_period == 0U ? 8U : ch->envelope_period;
}

static void noise_load_envelope(NoiseChannel* ch, uint8_t nr42) {
  ch->envelope_initial = (uint8_t)(nr42 >> 4);
  ch->volume = ch->envelope_initial;
  ch->envelope_increase = (nr42 & 0x08U) ? 1U : 0U;
  ch->envelope_period = (uint8_t)(nr42 & 0x07U);
  ch->envelope_counter = ch->envelope_period == 0U ? 8U : ch->envelope_period;
}

static void square_trigger(SquareChannel* ch, uint8_t duty, uint8_t nrx2, uint16_t freq) {
  square_update_dac(ch, nrx2);
  if (!ch->dac_enabled) return;

  ch->enabled = 1U;
  ch->duty = (uint8_t)(duty & 0x03U);
  ch->duty_pos = 0U;
  ch->frequency = (uint16_t)(freq & 0x07FFU);
  ch->timer = square_period(ch->frequency);
  if (ch->length_counter == 0U) ch->length_counter = 64U;
  square_load_envelope(ch, nrx2);
}

static void noise_trigger(NoiseChannel* ch, uint8_t nr42, uint8_t nr43) {
  noise_update_dac(ch, nr42);
  if (!ch->dac_enabled) return;

  ch->enabled = 1U;
  ch->clock_shift = (uint8_t)(nr43 >> 4);
  ch->width_mode = (nr43 & 0x08U) ? 1U : 0U;
  ch->divisor_code = (uint8_t)(nr43 & 0x07U);
  ch->timer = noise_period(ch->divisor_code, ch->clock_shift);
  ch->lfsr = 0x7FFFU;
  if (ch->length_counter == 0U) ch->length_counter = 64U;
  noise_load_envelope(ch, nr42);
}

static void square_tick(SquareChannel* ch) {
  if (!ch->enabled) return;
  if (ch->timer > 0U) ch->timer--;
  if (ch->timer == 0U) {
    ch->timer = square_period(ch->frequency);
    ch->duty_pos = (uint8_t)((ch->duty_pos + 1U) & 0x07U);
  }
}

static void noise_tick(NoiseChannel* ch) {
  uint16_t xored;

  if (!ch->enabled) return;
  if (ch->timer > 0U) ch->timer--;
  if (ch->timer != 0U) return;

  ch->timer = noise_period(ch->divisor_code, ch->clock_shift);
  xored = (uint16_t)((ch->lfsr & 0x01U) ^ ((ch->lfsr >> 1) & 0x01U));
  ch->lfsr = (uint16_t)((ch->lfsr >> 1) | (xored << 14));
  if (ch->width_mode) {
    ch->lfsr = (uint16_t)((ch->lfsr & ~(1U << 6)) | (xored << 6));
  }
}

static void square_clock_length(SquareChannel* ch) {
  if (ch->length_enabled && ch->length_counter > 0U) {
    ch->length_counter--;
    if (ch->length_counter == 0U) ch->enabled = 0U;
  }
}

static void noise_clock_length(NoiseChannel* ch) {
  if (ch->length_enabled && ch->length_counter > 0U) {
    ch->length_counter--;
    if (ch->length_counter == 0U) ch->enabled = 0U;
  }
}

static void square_clock_envelope(SquareChannel* ch) {
  if (!ch->enabled || ch->envelope_period == 0U) return;
  if (ch->envelope_counter > 0U) ch->envelope_counter--;
  if (ch->envelope_counter != 0U) return;

  ch->envelope_counter = ch->envelope_period;
  if (ch->envelope_increase) {
    if (ch->volume < 15U) ch->volume++;
  } else {
    if (ch->volume > 0U) ch->volume--;
  }
}

static void noise_clock_envelope(NoiseChannel* ch) {
  if (!ch->enabled || ch->envelope_period == 0U) return;
  if (ch->envelope_counter > 0U) ch->envelope_counter--;
  if (ch->envelope_counter != 0U) return;

  ch->envelope_counter = ch->envelope_period;
  if (ch->envelope_increase) {
    if (ch->volume < 15U) ch->volume++;
  } else {
    if (ch->volume > 0U) ch->volume--;
  }
}

static void clock_frame_sequencer(Apu* a) {
  if ((a->frame_seq_step & 1U) == 0U) {
    square_clock_length(&a->ch1);
    square_clock_length(&a->ch2);
    noise_clock_length(&a->ch4);
  }

  if (a->frame_seq_step == 7U) {
    square_clock_envelope(&a->ch1);
    square_clock_envelope(&a->ch2);
    noise_clock_envelope(&a->ch4);
  }

  a->frame_seq_step = (uint8_t)((a->frame_seq_step + 1U) & 0x07U);
}

static int16_t square_sample(const SquareChannel* ch) {
  int16_t amp;
  if (!ch->enabled || !ch->dac_enabled || ch->volume == 0U) return 0;
  amp = (int16_t)(ch->volume * 120);
  return kDuty[ch->duty][ch->duty_pos] ? amp : (int16_t)(-amp);
}

static int16_t noise_sample(const NoiseChannel* ch) {
  int16_t amp;
  if (!ch->enabled || !ch->dac_enabled || ch->volume == 0U) return 0;
  amp = (int16_t)(ch->volume * 80);
  return (ch->lfsr & 0x01U) ? (int16_t)(-amp) : amp;
}

static void ring_push(Apu* a, int16_t sample) {
  if (a->count >= RING_CAPACITY) {
    /* Drop oldest sample if host falls behind. Better than blocking emulation. */
    a->read_pos = (a->read_pos + 1U) % RING_CAPACITY;
    a->count--;
  }
  a->ring[a->write_pos] = sample;
  a->write_pos = (a->write_pos + 1U) % RING_CAPACITY;
  a->count++;
}

static void mix_channel(int32_t* left, int32_t* right, int16_t sample, uint8_t nr51,
                        uint8_t left_bit, uint8_t right_bit) {
  if (nr51 & left_bit) *left += sample;
  if (nr51 & right_bit) *right += sample;
}

static void produce_sample(Apu* a) {
  int32_t left = 0;
  int32_t right = 0;
  int32_t mix = 0;

  if (a->master_enable) {
    uint8_t nr50 = a->regs[reg_index(REG_NR50)];
    uint8_t nr51 = a->regs[reg_index(REG_NR51)];
    uint8_t left_vol = (uint8_t)(((nr50 >> 4) & 0x07U) + 1U);
    uint8_t right_vol = (uint8_t)((nr50 & 0x07U) + 1U);

    mix_channel(&left, &right, square_sample(&a->ch1), nr51, 0x10U, 0x01U);
    mix_channel(&left, &right, square_sample(&a->ch2), nr51, 0x20U, 0x02U);
    mix_channel(&left, &right, noise_sample(&a->ch4), nr51, 0x80U, 0x08U);

    left = (left * left_vol) / 8;
    right = (right * right_vol) / 8;
    mix = (left + right) / 2;
  }

  if (mix > 32767) mix = 32767;
  if (mix < -32768) mix = -32768;
  ring_push(a, (int16_t)mix);
}

static void power_off(Apu* a) {
  memset(a->regs, 0, sizeof(a->regs));
  memset(&a->ch1, 0, sizeof(a->ch1));
  memset(&a->ch2, 0, sizeof(a->ch2));
  memset(&a->ch4, 0, sizeof(a->ch4));
  a->master_enable = 0U;
}

Apu* apu_create(void) {
  Apu* a = calloc(1, sizeof(Apu));
  return a;
}

void apu_destroy(Apu* a) {
  free(a);
}

void apu_reset(Apu* a) {
  if (a == NULL) return;
  memset(a, 0, sizeof(*a));

  /* Post-boot-ish defaults: APU master on, output routes enabled. */
  a->master_enable = 1U;
  a->regs[reg_index(REG_NR50)] = 0x77U;
  a->regs[reg_index(REG_NR51)] = 0xFFU;
  a->regs[reg_index(REG_NR52)] = 0x80U;
  a->ch4.lfsr = 0x7FFFU;
}

void apu_tick_1t(Apu* a) {
  if (a == NULL) return;

  if (a->master_enable) {
    square_tick(&a->ch1);
    square_tick(&a->ch2);
    noise_tick(&a->ch4);

    a->frame_seq_counter++;
    if (a->frame_seq_counter >= FRAME_SEQ_PERIOD_T) {
      a->frame_seq_counter = 0U;
      clock_frame_sequencer(a);
    }
  }

  a->sample_accum_q16 += (1U << 16);
  if (a->sample_accum_q16 >= SAMPLE_PERIOD_Q16) {
    a->sample_accum_q16 -= SAMPLE_PERIOD_Q16;
    produce_sample(a);
  }
}

uint8_t apu_read(const Apu* a, uint16_t addr) {
  uint8_t v;
  if (a == NULL || addr < 0xFF10U || addr > 0xFF3FU) return 0xFF;

  if (addr == REG_NR52) {
    v = 0x70U;
    if (a->master_enable) v |= 0x80U;
    if (a->ch1.enabled) v |= 0x01U;
    if (a->ch2.enabled) v |= 0x02U;
    if (a->ch4.enabled) v |= 0x08U;
    return v;
  }

  return a->regs[reg_index(addr)];
}

void apu_write(Apu* a, uint16_t addr, uint8_t value) {
  uint8_t idx;
  if (a == NULL || addr < 0xFF10U || addr > 0xFF3FU) return;

  idx = reg_index(addr);

  if (addr == REG_NR52) {
    if (value & 0x80U) {
      a->master_enable = 1U;
      a->regs[idx] = 0x80U;
    } else {
      power_off(a);
    }
    return;
  }

  if (!a->master_enable) {
    /* Basic approximation: ignore writes while powered off. */
    return;
  }

  a->regs[idx] = value;

  switch (addr) {
    case REG_NR11:
      a->ch1.duty = (uint8_t)(value >> 6);
      a->ch1.length_counter = (uint16_t)(64U - (value & 0x3FU));
      break;
    case REG_NR12:
      square_update_dac(&a->ch1, value);
      a->ch1.envelope_initial = (uint8_t)(value >> 4);
      a->ch1.envelope_increase = (value & 0x08U) ? 1U : 0U;
      a->ch1.envelope_period = (uint8_t)(value & 0x07U);
      break;
    case REG_NR13:
      a->ch1.frequency = (uint16_t)((a->ch1.frequency & 0x0700U) | value);
      break;
    case REG_NR14:
      a->ch1.frequency = (uint16_t)((a->ch1.frequency & 0x00FFU) |
                                    ((uint16_t)(value & 0x07U) << 8));
      a->ch1.length_enabled = (value & 0x40U) ? 1U : 0U;
      if (value & 0x80U) {
        square_trigger(&a->ch1, (uint8_t)(a->regs[reg_index(REG_NR11)] >> 6),
                       a->regs[reg_index(REG_NR12)], a->ch1.frequency);
      }
      break;

    case REG_NR21:
      a->ch2.duty = (uint8_t)(value >> 6);
      a->ch2.length_counter = (uint16_t)(64U - (value & 0x3FU));
      break;
    case REG_NR22:
      square_update_dac(&a->ch2, value);
      a->ch2.envelope_initial = (uint8_t)(value >> 4);
      a->ch2.envelope_increase = (value & 0x08U) ? 1U : 0U;
      a->ch2.envelope_period = (uint8_t)(value & 0x07U);
      break;
    case REG_NR23:
      a->ch2.frequency = (uint16_t)((a->ch2.frequency & 0x0700U) | value);
      break;
    case REG_NR24:
      a->ch2.frequency = (uint16_t)((a->ch2.frequency & 0x00FFU) |
                                    ((uint16_t)(value & 0x07U) << 8));
      a->ch2.length_enabled = (value & 0x40U) ? 1U : 0U;
      if (value & 0x80U) {
        square_trigger(&a->ch2, (uint8_t)(a->regs[reg_index(REG_NR21)] >> 6),
                       a->regs[reg_index(REG_NR22)], a->ch2.frequency);
      }
      break;

    case REG_NR30:
      /* Wave channel is not generated yet; register is stored for compatibility. */
      break;

    case REG_NR41:
      a->ch4.length_counter = (uint16_t)(64U - (value & 0x3FU));
      break;
    case REG_NR42:
      noise_update_dac(&a->ch4, value);
      a->ch4.envelope_initial = (uint8_t)(value >> 4);
      a->ch4.envelope_increase = (value & 0x08U) ? 1U : 0U;
      a->ch4.envelope_period = (uint8_t)(value & 0x07U);
      break;
    case REG_NR43:
      a->ch4.clock_shift = (uint8_t)(value >> 4);
      a->ch4.width_mode = (value & 0x08U) ? 1U : 0U;
      a->ch4.divisor_code = (uint8_t)(value & 0x07U);
      a->ch4.timer = noise_period(a->ch4.divisor_code, a->ch4.clock_shift);
      break;
    case REG_NR44:
      a->ch4.length_enabled = (value & 0x40U) ? 1U : 0U;
      if (value & 0x80U) {
        noise_trigger(&a->ch4, a->regs[reg_index(REG_NR42)],
                      a->regs[reg_index(REG_NR43)]);
      }
      break;

    default:
      break;
  }
}

size_t apu_read_samples(Apu* a, int16_t* out, size_t max_samples) {
  size_t n = 0;
  if (a == NULL || out == NULL) return 0;

  while (n < max_samples && a->count > 0U) {
    out[n++] = a->ring[a->read_pos];
    a->read_pos = (a->read_pos + 1U) % RING_CAPACITY;
    a->count--;
  }
  return n;
}
