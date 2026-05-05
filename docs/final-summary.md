# Final Project Summary

## What Was Built

This project produced a low-level original Game Boy emulator written in C, plus a small original Game Boy game used as the final demonstration ROM.

The emulator models the major components needed to run DMG software:

- CPU instruction execution
- memory bus routing
- cartridge ROM/RAM access
- common cartridge controller behavior, including ROM-only, MBC1, MBC2, MBC3, and MBC5
- battery-backed RAM save/load through `.sav` files
- timer registers and timer interrupt behavior
- PPU LCD modes and scanline rendering
- joypad input
- OAM DMA
- serial output stub for tests
- basic approximate APU/audio output
- SDL frontend for interactive use
- headless frontend for tests and framebuffer dumps

The original game demonstrates that the emulator can run an interactive ROM with input, tile rendering, game state, random rolls, menus, battle logic, scene changes, and a complete win condition.

## Why This Is More Than a Normal Program

An emulator is not just an application that produces a screen. It is a software model of hardware. Small inaccuracies can break ROMs in ways that are not obvious from the code that failed.

The project required careful handling of:

- instruction timing
- memory access order
- interrupts
- LCD mode timing
- timer edge behavior
- cartridge banking
- battery-backed cartridge RAM
- sprite/background rendering rules
- input register behavior
- DMA restrictions
- host audio and frame pacing

This is why the project used both unit-style tests and real Game Boy test ROMs. Informal playtesting alone would not be enough.

## Scope Control

The project intentionally targets the original DMG Game Boy only.

Out of scope:

- Game Boy Color
- cycle-perfect APU/audio accuracy
- MBC3 real-time clock accuracy
- pixel-FIFO rendering
- full link cable
- save states
- debugger UI

These exclusions were not accidents. They were the difference between finishing a working emulator and constructing a glorious unfinished monument to overconfidence.

## Verification

Verification used a layered approach:

- SingleStepTests for CPU instruction state transitions
- Blargg ROMs for CPU behavior and timing
- internal tests for timer, MBC1, PPU, joypad, and DMA
- smoke testing for expanded cartridge support, battery saves, and audio output
- `dmg-acid2` for visual PPU correctness
- the original game for integrated interactive behavior

See `test-results.md` for the summarized pass/fail record.

## Main Challenges

### Timing Model

The hardest architectural issue was making time advance at the correct points. Returning a cycle count at the end of each instruction would be simpler, but it would let peripherals observe all memory accesses too late. The final design advances peripherals through bus accesses and explicit internal ticks.

### CPU Completeness

The CPU required a complete opcode table, CB-prefixed operations, interrupt behavior, delayed IME enable, stack behavior, and hardware quirks such as the HALT bug.

### PPU Tradeoffs

The project uses a scanline renderer instead of a pixel FIFO. This was a deliberate tradeoff: it supports the visual behavior needed for the demonstration and `dmg-acid2`, while avoiding weeks of pixel-fetcher timing work.

### Cartridge Compatibility

Cartridge support grew beyond the original no-MBC/MBC1 scope to include common MBC2, MBC3, and MBC5 behavior. This improved compatibility while keeping cartridge-specific logic contained inside the cartridge module.

### Integration

A playable ROM exercises more than one module at a time. The original game was useful because it tested input, rendering, frame pacing, random game state, scene transitions, menus, battle flow, and completion together.

## Final Deliverable

The final deliverable includes:

- emulator source code
- build and run instructions
- architecture documentation
- design decisions
- verification summary
- original Game Boy game source and manual, provided separately from this emulator repository
- playable ROM demonstration through the SDL frontend
