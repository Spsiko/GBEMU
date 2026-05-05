/**
 * @file serial.h
 * @brief Game Boy serial port stub (SB, SC).
 *
 * For now, the serial port is a write-only sink to stdout: when a
 * transfer is started by writing 0x80 or 0x81 to SC, the byte in SB
 * is printed and the transfer is marked complete. This is enough to
 * capture pass/fail messages from Blargg's test ROMs, which write
 * their result strings to the serial port.
 *
 * Real link-cable behavior (timing, two-side bit shifting, transfer
 * complete interrupt) is not modeled. When we add a real link cable
 * later, this module is the natural place for it.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

typedef struct Serial Serial;

Serial* serial_create(void);
void    serial_destroy(Serial* s);
void    serial_reset(Serial* s);

uint8_t serial_read(const Serial* s, uint16_t addr);
void    serial_write(Serial* s, uint16_t addr, uint8_t value);

#endif