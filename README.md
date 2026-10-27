# Game Boy Emulator

A low-level original Game Boy (DMG) emulator written in C. The project focuses on CPU execution, memory routing, cartridge loading, timer behavior, graphics output, joypad input, DMA, cartridge banking, battery-backed saves, and a small original Game Boy game used as the final demonstration ROM.

This is an honors project, so the goal is not just that a ROM window opens. The goal is that the emulator is organized around the same major hardware components as the original machine and is checked against known Game Boy test ROM behavior.

## Current Status

| Area | Status |
|---|---|
| CPU | Implemented, including main and CB-prefixed opcodes |
| Memory bus | Implemented, including WRAM, HRAM, Echo RAM, I/O routing, DMA gating, and interrupt register handling |
| Cartridge | ROM-only, ROM+RAM, MBC1, MBC2, MBC3, and MBC5 support for common cartridges |
| Battery saves | Battery-backed RAM loads from `.sav` files and writes on clean emulator exit |
| Timer | Implemented |
| PPU / graphics | Scanline renderer implemented |
| Joypad | Implemented |
| OAM DMA | Implemented |
| Serial | Stubbed for test ROM output |
| APU / audio | Functional approximate APU with square-wave and noise output; not cycle-perfect |
| Game Boy Color | Not implemented; DMG only |

See `docs/test-results.md` for verification notes.

## Project Structure

```text
src/        Emulator implementation files.
include/    Header files shared across emulator modules.
tests/      Internal tests, SingleStepTests support files, and test ROMs.
docs/       Architecture notes, design decisions, test results, and final summary.
build/      Generated object and dependency files (created by make).
bin/        Generated executables (created by make).
```

ROM files for commercial games are not included in this repository. To run the emulator, provide a legally obtained Game Boy ROM as a command-line argument.

## Build Environment

The emulator builds on Linux and on Windows (MinGW/MSYS2). The Makefile detects which one it is running on.

Both platforms need:

- `gcc`
- `make`
- SDL2 development headers and libraries, for the SDL frontend only

The headless emulator has no dependencies beyond the C standard library, so `make headless` works even without SDL2 installed.

### Linux

Tested on Ubuntu 24.04 with SDL2 2.30.

On Debian or Ubuntu, install the compiler, Make, and SDL2 with:

```sh
sudo apt install build-essential libsdl2-dev
```

On other distributions, install your package manager's SDL2 development package. The Makefile finds SDL2 through `sdl2-config`, so check that it is available:

```sh
sdl2-config --version
```

No other setup is needed.

### Windows (MSYS2)

Check that the compiler and Make are available:

```sh
which gcc
which make
gcc --version
make --version
```

If using MSYS2 MinGW64 and these tools are missing, install them with:

```sh
pacman -S make mingw-w64-x86_64-gcc
```

The SDL frontend also requires SDL2 headers and libraries. By default, the Makefile expects the SDL2 MinGW development folder to be located next to the emulator folder:

```text
emu/
  Emulator/
  x86_64-w64-mingw32/
    include/
      SDL2/
        SDL.h
    lib/
      libSDL2.dll.a
      libSDL2main.a
```

If SDL2 is installed somewhere else, pass its location to make instead of editing the Makefile:

```sh
make SDL_DIR=/path/to/x86_64-w64-mingw32
```

## Build

From the project root, build both the headless emulator and the SDL frontend:

```sh
make
```

To build only one of them:

```sh
make headless
make sdl
```

The executables are written to `bin/`:

| Platform | Headless | SDL frontend |
|---|---|---|
| Linux | `bin/emu` | `bin/emu_sdl` |
| Windows | `bin/emu.exe` | `bin/emu_sdl.exe` |

## Cleaning Build Files

To remove all build output:

```sh
make clean
```

This removes both `build/` and `bin/`. On Windows, that includes any `SDL2.dll` copied into `bin/`, so copy it back after cleaning.

## SDL2 Runtime Note (Windows)

On Windows, the SDL frontend needs `SDL2.dll` at runtime.

Keep `SDL2.dll` in the same folder as `emu_sdl.exe`:

```text
bin/
  emu_sdl.exe
  SDL2.dll
```

`SDL2.dll` is a runtime dependency. It is separate from the SDL2 headers and libraries used while compiling. The DLL is not intended to be tracked in git; include it only in a local demo/release folder if someone else needs to run the executable.

If `SDL2.dll` is missing, Windows may report that the program cannot start because `SDL2.dll` was not found.

On Linux, the SDL2 library installed from your package manager is used automatically.

## Run

The examples below use the Linux executable names. On Windows, add `.exe` (for example `./bin/emu.exe`).

### Headless

```sh
./bin/emu path/to/rom.gb
```

Useful options:

```sh
./bin/emu path/to/rom.gb --max-frames 7200
./bin/emu path/to/rom.gb --max-frames 300 --dump-framebuffer out.ppm
```

The serial stub prints serial output to stdout, which is useful for Blargg-style test ROMs. For a quick check that the build works, run the included Blargg CPU test:

```sh
./bin/emu tests/test_roms/cpu_instrs.gb
```

It should end with `Passed all tests`.

### SDL

```sh
./bin/emu_sdl path/to/rom.gb
```

You can also build and run in one step:

```sh
make run ROM=path/to/rom.gb
make run-sdl ROM=path/to/rom.gb
```

Keyboard controls:

| Key | Game Boy Button |
|---|---|
| Arrow keys | D-pad |
| Z | A |
| X | B |
| Enter | Start |
| Backspace | Select |
| Esc | Quit emulator |

## Battery Saves

For battery-backed cartridges, the emulator derives a save path from the ROM filename.

Examples:

```text
zelda.gb      -> zelda.sav
pokemon.gbc   -> pokemon.sav
```

The `.sav` file is loaded when the ROM starts and written when the emulator exits cleanly. Force-closing the program may lose unsaved SRAM changes.

## Original Demo Game

The emulator was demonstrated with a small original Game Boy board game built with GBDK. It shows that the emulator can run an original ROM with input, tile graphics, random rolls, scene transitions, menus, combat, and a complete win condition.

The game is not included in this repository.

## Testing

The project uses a mix of internal tests and ROM-level diagnostic tests.

Important verification areas include:

- SingleStepTests support for CPU instruction behavior
- Blargg CPU/timing ROMs
- timer tests
- cartridge/MBC tests
- PPU rendering, sprite, and timing tests
- `dmg-acid2` for visual PPU behavior
- joypad tests
- DMA tests
- manual SDL validation
- manual original-game playthrough

Some older development harnesses under `tests/single_step/` are retained for reference and may require their own runner setup.

See `docs/test-results.md` for a summarized pass/fail record.

## Documentation

| File | Purpose |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | How the emulator is structured |
| [`docs/decisions.md`](docs/decisions.md) | Major scope and architecture decisions |
| [`docs/test-results.md`](docs/test-results.md) | Verification summary |
| [`docs/final-summary.md`](docs/final-summary.md) | Short final-project summary and challenges |

## Known Limitations

- DMG only; Game Boy Color-specific behavior is out of scope.
- The APU is functional but approximate. It supports basic square-wave and noise audio, but it is not cycle-perfect.
- MBC3 real-time clock behavior is recognized but not fully implemented.
- PPU uses a scanline renderer, not a pixel FIFO.
- Mode 3 timing is fixed rather than sprite/window-variable.
- Serial is a test-output stub, not a full link-cable implementation.
- No save states or debugger UI.

## References

- Pan Docs
- Blargg Game Boy test ROMs
- SingleStepTests SM83
- dmg-acid2
- SDL2
