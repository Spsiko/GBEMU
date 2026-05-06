# Design Decisions

This file records the main scope and architecture decisions for the emulator. It is not a diary of every bug fix. Future humanity, if it survives this codebase, only needs the decisions that explain why the emulator is shaped this way.

---

## 001. Target DMG only

**Decision:** Emulate the original Game Boy / DMG only. Do not implement Game Boy Color features.

**Why:** CGB support adds palettes, double speed, additional VRAM behavior, CGB-specific priority rules, and a wider compatibility/test surface. None of that is required for the final test ROM set or the original demo game.

**Alternatives considered:** Add CGB compatibility. Rejected as scope bloat.

---

## 002. CPU at M-cycle granularity, peripherals at T-cycle granularity

**Decision:** CPU instructions are modeled at M-cycle granularity. Timer, PPU, serial, DMA, and APU are ticked at T-cycle granularity through `sys_tick`.

**Why:** The CPU naturally spends time through memory accesses and internal M-cycles. Peripherals need finer timing to behave correctly around timer edges, LCD mode changes, DMA, audio sample generation, and interrupts.

**Alternatives considered:** Instruction-stepped timing. Rejected because it cannot correctly interleave peripherals between memory accesses inside one instruction.

---

## 003. `sys_tick` lives on the bus

**Decision:** The system scheduler lives in the bus module.

**Why:** The bus already owns or references every peripheral because it routes memory accesses. Hosting the scheduler there avoids coupling the CPU directly to every device.

**Alternatives considered:** Put ticking in the CPU or a separate scheduler module. CPU-hosted ticking couples unrelated modules; a scheduler module is cleaner on paper but unnecessary at this scale.

---

## 004. No boot ROM

**Decision:** Do not run the original DMG boot ROM. `cpu_reset` initializes registers to documented post-boot values and starts execution at `0x0100`.

**Why:** The boot ROM requires another ROM dependency and a temporary memory mapping at `0x0000-0x00FF`. Test ROMs and commercial ROMs normally assume post-boot state. Boot ROM emulation is polish, not a grading requirement.

**Alternatives considered:** Implement boot ROM mapping and execution. Rejected as extra scope.

---

## 005. APU accuracy is limited

**Decision:** Implement functional but approximate audio rather than a cycle-perfect Game Boy APU.

**Why:** A correct APU is its own substantial subsystem: four channels, frame sequencer, envelopes, sweeps, length counters, mixing, host audio output, and hardware-specific quirks. The emulator can demonstrate CPU, memory, graphics, timing, input, and compatibility without perfect audio. A basic APU still improves the interactive demo.

**Accepted limitations:** Audio should be described as approximate. It supports useful square-wave/noise output, but not full hardware-accurate behavior.

**Alternatives considered:** Implement fully accurate audio. Rejected as too large for the timeframe. Leave audio completely silent. Rejected once the main emulator and demo game were stable enough to add limited sound.

---

## 006. Common cartridge controller support

**Decision:** Support the common cartridge controllers needed for broader DMG compatibility: ROM-only, ROM+RAM, MBC1, MBC2, MBC3, and MBC5. Battery-backed RAM is persisted through `.sav` files.

**Why:** No-MBC and MBC1 cover many tests and simple ROMs, but broader compatibility benefits from MBC2, MBC3, and MBC5 support. Keeping this logic inside the cartridge module preserves the CPU/bus separation.

**Accepted limitations:** MBC3 real-time clock behavior is recognized but not fully implemented. More obscure hardware such as MMM01, MBC6, MBC7, HuC variants, camera hardware, and other specialty cartridges remain out of scope.

**Alternatives considered:** Stop at no-MBC/MBC1. Rejected after the emulator had a stable base and cartridge expansion could be added locally. Implement every known cartridge type. Rejected as compatibility sprawl.

---

## 007. Hybrid opcode dispatch

**Decision:** Use opcode function tables with a hybrid strategy: shared decode handlers for regular families and individual functions for distinct behavior.

**Why:** One function per opcode is repetitive. Pure decoding makes special instructions harder to read. Hybrid dispatch keeps the common families compact while preserving clarity for jumps, calls, stack operations, interrupts, and special cases.

**Alternatives considered:** Giant switch statement or one function per opcode. Rejected for readability and maintenance.

---

## 008. Opcodes spend cycles directly

**Decision:** Opcode functions do not return cycle counts. They spend cycles as they execute by calling `bus_read`, `bus_write`, or `sys_tick`.

**Why:** Returning a total cycle count at the end of an instruction delays all peripheral effects until the instruction finishes. Real hardware advances during the instruction.

**Alternatives considered:** Return cycle count from each opcode. Rejected as instruction-stepped and too coarse.

---

## 009. Main and CB opcode tables use designated initializers

**Decision:** Opcode tables use C99 designated initializers.

**Why:** The byte-to-function mapping is explicit and visually checkable. Missing slots remain `NULL`, causing a deliberate abort if an unimplemented opcode is reached.

**Alternatives considered:** Runtime table fill or switch dispatch. Rejected as easier to get wrong.

---

## 010. CPU split across `cpu.c`, `cpu_ops.c`, and `cpu_internal.h`

**Decision:** CPU lifecycle/step logic, opcode implementations, and private CPU structure live in separate files.

**Why:** Complete CPU support is large. Splitting keeps `cpu.c` readable while keeping the public CPU API small.

**Alternatives considered:** Single large `cpu.c`. Rejected as annoying to navigate, and therefore guaranteed to breed bugs.

---

## 011. IME as a three-state enum

**Decision:** Model interrupt master enable as `DISABLED`, `ENABLING`, and `ENABLED`.

**Why:** `EI` enables interrupts after the following instruction, not immediately. A three-state enum expresses that delay directly.

**Alternatives considered:** Boolean plus pending flag. Equivalent but less clear.

---

## 012. HALT bug implemented

**Decision:** Implement the documented HALT bug where a pending interrupt with IME disabled causes the next opcode fetch to not increment PC.

**Why:** It is real hardware behavior and is tested by accuracy suites.

**Alternatives considered:** Ignore it. Rejected as too visibly wrong for a cycle-aware emulator.

---

## 013. STOP behavior follows the current CPU test scope

**Decision:** Implement STOP behavior within the emulator's current DMG execution model and test scope.

**Why:** STOP has low-power hardware behavior that is not central to the final demonstration. The implementation focuses on behavior needed by the CPU tests and project scope rather than modeling every hardware-side power state.

**Alternatives considered:** Treat STOP as a full hardware power-state transition. Rejected as unnecessary for the current emulator scope.

---

## 014. Interrupt requests go through `bus_request_interrupt`

**Decision:** Peripherals request interrupts through `bus_request_interrupt(bus, INT_X)` instead of manually writing IF.

**Why:** Peripherals should not hardcode the IF register address or bit layout. The bus already owns interrupt register storage.

**Alternatives considered:** Direct `0xFF0F` read-modify-write in each peripheral. Rejected as duplicated magic-number soup.

---

## 015. Frame pacing belongs to the host

**Decision:** `emu_run_frame` advances one frame of emulated time and returns. It does not sleep.

**Why:** Tests want maximum speed. Interactive play wants real-time pacing. Keeping pacing in the host allows both without changing the emulator core.

**Alternatives considered:** Sleep inside `emu_run_frame`. Rejected because it makes tests slow and mixes host policy into hardware emulation.

---

## 016. Two host programs

**Decision:** Provide a headless host (`main.c`) and an SDL host (`main_sdl.c`).

**Why:** The headless host is better for tests, serial output, and framebuffer dumps. The SDL host is better for interactive games, input testing, audio output, and the final demonstration.

**Alternatives considered:** One unified executable with modes. Rejected as extra option plumbing for little benefit.

---

## 017. `cart_load_from_buffer` for tests

**Decision:** The cart module supports loading from both a file path and an in-memory byte buffer.

**Why:** Tests can synthesize ROM data without creating temp files. This avoids Windows/POSIX temp-file differences and makes tests simpler.

**Alternatives considered:** Temp files or `FILE*` inputs. Rejected as clumsier.

---

## 018. PPU uses scanline rendering

**Decision:** Render a full visible scanline at mode 3 entry.

**Why:** It is much simpler than a pixel FIFO and produces correct output for the target tests and demo game. CPU access to VRAM is blocked during mode 3, so most software cannot observe the difference.

**Alternatives considered:** Pixel FIFO renderer. Rejected as a major scope increase.

---

## 019. Mode 3 has fixed length

**Decision:** Mode 3 is fixed at 172 T-cycles, with mode 0 using the remaining scanline time.

**Why:** Real mode 3 length varies with sprites/window behavior. Modeling that accurately fits naturally with a pixel FIFO, not a scanline renderer. Fixed timing is an accepted limitation.

**Alternatives considered:** Compute variable mode 3 length separately. Rejected as high complexity for little final-demo value.

---

## 020. Window line counter advances only when the window draws

**Decision:** The PPU tracks a separate window line counter and increments it only on scanlines where the window actually contributes pixels.

**Why:** Real hardware behaves this way. Using `LY - WY` directly causes visible mistakes when the window is disabled and re-enabled mid-frame.

**Alternatives considered:** Use `LY - WY`. Rejected as visually wrong.

---

## 021. DMG sprite priority rules

**Decision:** For overlapping sprites, smaller X wins; if X matches, lower OAM index wins.

**Why:** This is DMG behavior. CGB uses different rules and is out of scope.

**Alternatives considered:** CGB ordering or sorting by OAM only. Rejected as wrong target hardware.

---

## 022. OBJ-over-BG checks pre-palette BG color index

**Decision:** Sprite priority against BG/window uses the background color index before palette mapping.

**Why:** Hardware priority is based on color index, not final displayed shade. Palette tricks should not change priority behavior.

**Alternatives considered:** Check framebuffer shade. Rejected as wrong.

---

## 023. BG disabled draws white on DMG

**Decision:** If LCDC bit 0 disables the background, the DMG renderer outputs color 0/white for the BG layer.

**Why:** This matches DMG behavior. CGB semantics differ but are out of scope.

**Alternatives considered:** Treat disabled BG as transparent. Rejected as CGB-style behavior.

---

## 024. Enforce 10 sprites per scanline

**Decision:** OAM scan selects at most 10 sprites per visible scanline.

**Why:** This is a real hardware limit, and games can depend on it.

**Alternatives considered:** Draw every matching sprite. Rejected as inaccurate.

---

## 025. Test-only VRAM/OAM poke helpers

**Decision:** The PPU exposes internal no-gate helpers for test setup.

**Why:** Tests need to set VRAM/OAM to known values without choreographing LCD state. Production CPU paths still use gated reads/writes.

**Alternatives considered:** Force tests through normal LCD timing. Rejected as noisy and brittle.

---

## 026. Joypad host API is an idempotent bitmask setter

**Decision:** Host code sets the current state of all buttons with a bitmask.

**Why:** SDL naturally reports current key state. The joypad module can internally detect transitions and raise interrupts.

**Alternatives considered:** Host-level press/release events. Rejected as duplicated transition logic.

---

## 027. DMA reads source through internal no-tick access

**Decision:** DMA uses internal bus access to read source bytes and PPU poke access to write OAM.

**Why:** DMA is not a CPU memory access. It should not be blocked by the CPU access restrictions caused by DMA itself.

**Alternatives considered:** Duplicate memory routing in DMA. Rejected as worse maintenance.

---

## 028. DMA gates CPU access during transfer

**Decision:** During DMA, CPU access is restricted. HRAM, IE, and the DMA register remain accessible; other regions read as `0xFF` or drop writes.

**Why:** This approximates documented hardware behavior while preserving practical interrupt handling.

**Alternatives considered:** No gating or strict HRAM-only gating. Rejected as either too permissive or too restrictive.

---

## 029. Serial is a test-output stub

**Decision:** Serial does not implement a full link cable. It captures/prints test-ROM output.

**Why:** Blargg-style tests use serial output for pass/fail text. Full link behavior is a different feature.

**Alternatives considered:** Network link cable. Rejected for the final deliverable.

---

## 030. SDL frontend uses host-side pacing

**Decision:** The SDL frontend runs one emulated frame, presents the framebuffer, queues available audio samples, then waits until the next frame deadline.

**Why:** This keeps emulator timing deterministic and lets the host handle OS sleep granularity, presentation details, and SDL audio output.

**Alternatives considered:** Put pacing into the emulator core. Rejected by decision 015.

---

## 031. Framebuffer stores grayscale indices

**Decision:** The PPU framebuffer stores 0-3 grayscale indices. Hosts convert those to RGB for output.

**Why:** The emulator core should represent Game Boy output, not SDL-specific pixels. The headless and SDL hosts can share the same framebuffer.

**Alternatives considered:** Store RGB in the PPU. Rejected as host-specific.

---

## 032. Assert internal contracts, validate external data

**Decision:** Internal caller contracts use asserts. External data, such as ROM files, is validated at the boundary and reported through return codes.

**Why:** Null-checking every internal pointer hides bugs and clutters code. Asserting on user-controlled data is also wrong. The split keeps both concerns honest.

**Alternatives considered:** Defensive-check everything or assert everything. Both rejected because they confuse bug detection with user-input handling.

---

## 033. Original game is the final interactive demonstration ROM

**Decision:** The project uses a small original Game Boy game as the final demonstration ROM.

**Why:** Test ROMs prove specific hardware behaviors, but the game proves the emulator can run an interactive program with input, graphics, game state, menus, and completion.

**Alternatives considered:** Only demonstrate commercial ROMs/test ROMs. Rejected because the rubric includes an original game demonstration.

---

## 034. Battery-backed RAM is saved as `.sav` on clean exit

**Decision:** Battery-backed cartridge RAM is loaded from a `.sav` file when the ROM starts and written back when the emulator exits normally.

**Why:** This matches common emulator behavior without adding save states or complicated host-side save management. The save file uses the same base name as the ROM, so `game.gb` maps to `game.sav`.

**Accepted limitations:** Force-closing the process may lose SRAM changes from the current run.

**Alternatives considered:** Autosave periodically or save when RAM is disabled. Rejected as extra behavior for the current scope.

---

## 035. Basic APU added as audible-output layer

**Decision:** Add a functional approximate APU implementation that supports useful square-wave and noise output and connects it to SDL queued audio. Full audio accuracy remains out of scope.

**Why:** The original deliverable did not require perfect audio, but a basic APU improves the emulator demonstration without reopening the entire audio subsystem. The implementation is hosted in `apu.c`/`apu.h`, ticked from `sys_tick`, and exposed to the SDL host through `emu_audio_read`. This keeps SDL-specific audio output out of the core emulator.

**Accepted limitations:** The APU is approximate. Channel 1 sweep, exact frame-sequencer edge cases, analog filtering, and complete wave-channel behavior are not treated as final-scope requirements.

**Alternatives considered:** Keep audio entirely unimplemented. Rejected because limited audio is useful polish once the main emulator and game are stable. Implement full APU accuracy. Rejected as too large for the remaining timeframe.
