# Test Results

This file summarizes the emulator verification story for the final project. It is meant to be read by a grader, not worshipped as a sacred artifact. Rerun paths depend on where the test ROMs live on your machine.

## Summary

| Area | Test / Evidence | Result | Notes |
|---|---|---|---|
| CPU opcodes | SingleStepTests SM83 | Pass | Full implemented CPU table checked |
| CPU instruction behavior | Blargg `cpu_instrs.gb` | Pass | 11/11 subtests recorded |
| CPU timing | Blargg `instr_timing.gb` | Pass | Recorded pass |
| Timer | Internal timer tests | Pass | DIV/TIMA/TMA/TAC behavior |
| MBC1 | Internal MBC1 tests | Pass | ROM/RAM banking |
| MBC2 | Manual smoke testing | Pass / smoke tested | Basic compatibility check |
| MBC3 | Manual smoke testing | Pass / partial | ROM/RAM banking; RTC not fully implemented |
| MBC5 | Manual smoke testing | Pass / smoke tested | Basic compatibility check |
| Battery saves | Manual save/reload validation | Pass | `.sav` loaded on start and written on clean exit |
| PPU rendering | Internal PPU tests | Pass | Background/window/sprite behavior within scanline-renderer scope |
| PPU visual accuracy | `dmg-acid2` | Pass | Recorded visual pass |
| Joypad | Internal joypad tests | Pass | Register behavior and interrupt behavior |
| OAM DMA | Internal DMA test | Pass | OAM transfer and CPU access gating |
| Original game | Original GBDK board game | Pass | Runs to completion on `emu_sdl` |
| APU / audio | Manual SDL validation | Pass / approximate | Square-wave and noise output; not cycle-perfect |

## Verification Approach

The project uses three levels of verification:

1. **Fine-grained CPU tests.** SingleStepTests checks individual CPU instructions against expected state transitions.
2. **ROM-level diagnostic tests.** Blargg test ROMs exercise CPU behavior and timing through a real ROM execution path.
3. **Visual/interactive validation.** `dmg-acid2` checks PPU behavior, and the original game checks the emulator as an interactive system.

This mix matters because one test style does not catch everything. A CPU unit test can prove an opcode is correct in isolation, but it cannot prove the frame loop, PPU, joypad, cartridge path, and audio path work together. A playable ROM proves integration, but not every weird CPU flag edge case. Annoying, yes. Necessary, also yes.

## Headless Test Runner

The headless binary runs without real-time pacing and prints serial output to stdout.

Example:

```sh
./bin/emu.exe tests/test_roms/cpu_instrs.gb --max-frames 7200
```

To dump the framebuffer after running:

```sh
./bin/emu.exe tests/test_roms/dmg-acid2.gb --max-frames 300 --dump-framebuffer acid2.ppm
```

## SDL Validation

The SDL frontend is used for interactive testing:

```sh
./bin/emu_sdl.exe path/to/game.gb
```

The original game was used to validate:

- title/game-over flow
- keyboard-to-joypad mapping
- tile rendering
- background updates
- frame pacing
- scene transitions
- random rolls
- menu input
- board movement
- battle flow
- audio output at a basic functional level

## Cartridge and Save Validation

Expanded cartridge support was checked through a mix of internal tests and manual smoke tests. The goal was to confirm that representative ROMs boot and reach normal gameplay behavior for the common MBC families now supported by the cartridge module.

Battery-backed saves were checked by saving in-game, exiting the emulator normally, confirming that a `.sav` file was created next to the ROM, and relaunching the ROM to confirm the save data loaded.

## Known Non-Goals

The following are not failures for this project:

- Cycle-perfect audio accuracy: APU output is functional but approximate.
- MBC3 real-time clock accuracy: RTC behavior is recognized but not fully implemented.
- Game Boy Color behavior: CGB is out of scope.
- Mooneye PPU tests depending on variable mode 3 length or pixel-FIFO details: scanline rendering is the documented scope.
- Full link-cable behavior: serial is only a test-output stub.
