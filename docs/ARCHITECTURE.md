# Emulator Architecture

This document describes how the emulator is organized and where the major hardware responsibilities live. It is intentionally practical rather than encyclopedic. Pan Docs remains the hardware reference; this file explains how this project models that hardware.

## Goal

The emulator targets the original Game Boy, also called the DMG. It is written in C and is built around a CPU, memory bus, cartridge, timer, PPU, joypad, DMA unit, serial stub, and a basic APU.

The emulator does not implement Game Boy Color features. Audio support is intentionally limited: the SDL frontend can play approximate square-wave and noise output, but full APU accuracy is out of scope.

## High-Level Structure

```text
main.c / main_sdl.c
        |
        v
      Emu
        |
        +-- CPU
        |     |
        |     v
        |   Bus
        |     +-- Cart
        |     +-- Timer
        |     +-- PPU
        |     +-- Joypad
        |     +-- DMA
        |     +-- APU
        |     +-- Serial
        |     +-- WRAM / HRAM
        |
        +-- host-facing frame loop
```

The CPU does not talk directly to the peripherals. It talks to the bus. The bus routes reads and writes to the correct hardware module.

This keeps the CPU focused on instruction behavior and keeps the memory map in one place, which is good because hardware bugs already provide enough suffering without duplicating address logic everywhere.

## Source Layout

```text
src/        Emulator implementation files.
include/    Header files shared across emulator modules.
tests/      Internal tests, SingleStepTests runner files, and test ROM assets.
docs/       Architecture notes, design decisions, test results, and final summary.
bin/        Local executable output and SDL2.dll for local Windows SDL runs.
```

## Clock Model

The Game Boy runs at 4.194304 MHz. This project treats CPU instructions at M-cycle granularity, where:

```text
1 M-cycle = 4 T-cycles
```

Peripherals are ticked at T-cycle granularity through the bus scheduler.

### Central Invariant

Before a CPU memory access completes, the rest of the system must be advanced to the point where real hardware would have been at that access.

This is handled by calling `sys_tick` before normal bus reads and writes.

```c
uint8_t bus_read(Bus* b, uint16_t addr) {
    sys_tick(b, 1);
    return dispatch_read(b, addr);
}

void bus_write(Bus* b, uint16_t addr, uint8_t v) {
    sys_tick(b, 1);
    dispatch_write(b, addr, v);
}
```

Opcodes spend cycles by performing bus accesses or by explicitly ticking internal cycles when an instruction has time cost but no memory access.

## Frame Loop

The top-level emulator frame runner advances one DMG frame of time:

```c
void emu_run_frame(Emu* e) {
    uint64_t start = bus_total_t_cycles(e->bus);
    uint64_t deadline = start + 70224;
    while (bus_total_t_cycles(e->bus) < deadline) {
        cpu_step(e->cpu);
    }
}
```

The emulator core does not sleep. Frame pacing belongs to the host program.

- `main.c` runs as fast as possible for tests and framebuffer dumps.
- `main_sdl.c` paces frames for real-time interactive use.

## Memory Map

```text
0x0000 - 0x3FFF : Cartridge ROM bank 0
0x4000 - 0x7FFF : Cartridge ROM switchable bank
0x8000 - 0x9FFF : VRAM
0xA000 - 0xBFFF : Cartridge RAM
0xC000 - 0xDFFF : WRAM
0xE000 - 0xFDFF : Echo RAM mirror
0xFE00 - 0xFE9F : OAM
0xFEA0 - 0xFEFF : Unusable memory
0xFF00 - 0xFF7F : I/O registers
0xFF80 - 0xFFFE : HRAM
0xFFFF          : IE interrupt-enable register
```

The bus owns WRAM, HRAM, interrupt registers, and memory routing. Hardware modules own their own mapped regions where appropriate.

## CPU

The CPU module handles:

- post-bootrom reset state
- fetch/decode/execute
- main opcode table
- CB-prefixed opcode table
- interrupts
- IME delayed enable behavior
- HALT and the HALT bug
- STOP behavior within the emulator's current DMG execution model and test scope

The CPU is split into:

| File | Purpose |
|---|---|
| `include/cpu.h` | Public CPU API |
| `include/cpu_internal.h` | Private CPU state, registers, helpers |
| `src/cpu.c` | CPU lifecycle, stepping, interrupts, opcode tables |
| `src/cpu_ops.c` | Opcode implementations |

Opcodes have this shape:

```c
void op_xx(CPU* c, uint8_t opcode)
```

They do not return cycle counts. Instead, they spend cycles as they run. This avoids the broken instruction-stepped model where all peripheral effects are delayed until the instruction ends. The machine does not politely wait until your function returns. Rude, but historically accurate.

## Bus

The bus routes memory accesses and owns the system scheduler.

Responsibilities:

- dispatch reads/writes by address
- tick clocked peripherals
- expose `bus_peek` / `bus_poke` for non-ticking internal access
- expose interrupt request helper
- own WRAM and HRAM
- implement Echo RAM behavior
- implement unusable memory behavior
- coordinate DMA gating

`bus_peek` and `bus_poke` are deliberately not normal CPU accesses. They are for internal emulator observations and hardware paths that must not advance time.

## Cartridge

The cartridge module owns ROM data, cartridge RAM, MBC state, and battery-backed save files.

Supported cartridge behavior includes:

- ROM-only carts
- ROM + RAM carts
- MBC1 ROM/RAM banking
- MBC2 ROM banking and internal RAM behavior
- MBC3 ROM/RAM banking
- MBC5 ROM/RAM banking
- ROM header validation
- loading from file
- loading from memory buffer for tests
- battery-backed RAM save/load through `.sav` files

MBC3 real-time clock registers are recognized as a limitation and are not fully implemented.

## Timer

The timer module implements:

- DIV
- TIMA
- TMA
- TAC
- timer interrupt request

The timer is ticked at T-cycle granularity. TIMA increments on the falling edge of selected bits of the internal counter, based on TAC. This matters because a simple “increment every N cycles” model misses edge cases.

## PPU

The PPU implements the LCD mode state machine and scanline rendering.

Responsibilities:

- VRAM
- OAM
- LCDC / STAT / LY / LYC and related registers
- mode transitions
- STAT interrupt behavior
- VBlank interrupt behavior
- background rendering
- window rendering
- sprite rendering
- framebuffer output

Rendering is scanline-based: one visible scanline is rendered as a batch when mode 3 begins.

This is simpler than a pixel FIFO and is enough for the project’s verification and demo goals. The accepted tradeoff is that some extremely specific mid-scanline timing effects are out of scope.

## Joypad

The joypad module implements the JOYP register and button interrupt behavior.

The host provides the current state of all buttons as a bitmask. The joypad diffs that state internally to decide when an interrupt should fire.

The SDL host maps keyboard input to the bitmask once per frame.

## DMA

The DMA module implements OAM DMA initiated through `0xFF46`.

It copies 160 bytes from the selected source page to OAM. During DMA, CPU reads/writes outside the allowed regions are gated by the bus.

DMA uses internal no-tick accessors so the transfer itself does not get blocked by the CPU access restrictions it creates.

## APU

The APU is functional but approximate. It is ticked from `sys_tick`, stores its own register state and sample ring buffer, and is drained by the SDL host through `emu_audio_read`.

Implemented audio behavior includes basic square-wave output and noise support. Full APU accuracy is out of scope, including exact sweep behavior, cycle-perfect frame-sequencer edge cases, analog filtering, and full wave-channel behavior.

## Serial

Serial is implemented as a stub for test ROM output.

When a ROM writes bytes to the serial registers and starts a transfer, the stub can print output to stdout. This is enough for Blargg-style test ROMs that report status over serial.

Full link-cable emulation is out of scope.

## Host Programs

### `main.c`

Headless runner.

Use cases:

- run test ROMs quickly
- capture serial output
- dump framebuffer to PPM

Options:

```text
--max-frames N
--dump-framebuffer FILE
```

### `main_sdl.c`

Interactive SDL2 frontend.

Use cases:

- run playable ROMs
- test joypad input
- demonstrate the original game

It converts the PPU’s 0-3 grayscale framebuffer into RGB pixels, uploads them to an SDL texture, drains APU samples into SDL queued audio, and paces frames in real time.

## Out of Scope

- Game Boy Color
- cycle-perfect APU behavior
- MBC3 real-time clock accuracy
- pixel FIFO rendering
- variable mode 3 length
- full link cable
- save states
- debugger UI
