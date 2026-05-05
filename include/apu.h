/**
 * @file apu.h
 * @brief Minimal DMG audio processing unit implementation.
 *
 * This is intentionally a basic APU, not a cycle-perfect audio core.
 * It implements channels 1 and 2 (square waves) plus channel 4
 * (noise), with approximated length counters and envelopes. Wave
 * output and channel 1 sweep are still stubbed/approximated.
 */
#ifndef APU_H
#define APU_H

#include <stddef.h>
#include <stdint.h>

typedef struct Apu Apu;

Apu* apu_create(void);
void apu_destroy(Apu* a);
void apu_reset(Apu* a);

/** Tick the APU forward by one DMG T-cycle. */
void apu_tick_1t(Apu* a);

/** Memory-mapped register access for 0xFF10-0xFF3F. */
uint8_t apu_read(const Apu* a, uint16_t addr);
void apu_write(Apu* a, uint16_t addr, uint8_t value);

/**
 * @brief Pull mono signed 16-bit samples from the APU ring buffer.
 *
 * Returns the number of samples copied. If fewer samples are available
 * than requested, the caller should fill the remainder with silence.
 */
size_t apu_read_samples(Apu* a, int16_t* out, size_t max_samples);

#endif /* APU_H */
