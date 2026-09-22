CC := gcc

CFLAGS := -std=c11 -Wall -Wextra -O2 -Iinclude
DEPFLAGS := -MMD -MP
LDLIBS :=

# MinGW/MSYS2 on Windows sets OS=Windows_NT. WSL and Linux do not.
ifeq ($(OS),Windows_NT)
  EXE := .exe
  SDL_DIR ?= ../x86_64-w64-mingw32
  SDL_CFLAGS := -I$(SDL_DIR)/include
  SDL_LDLIBS := -L$(SDL_DIR)/lib -lmingw32 -lSDL2main -lSDL2
else
  EXE :=
  SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
  SDL_LDLIBS := $(shell sdl2-config --libs 2>/dev/null)
endif

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := bin

CORE_SRC := \
  src/emu.c \
  src/cart.c \
  src/bus.c \
  src/timer.c \
  src/serial.c \
  src/ppu.c \
  src/joypad.c \
  src/dma.c \
  src/apu.c \
  src/cpu.c \
  src/cpu_ops.c

CORE_OBJ := $(CORE_SRC:src/%.c=$(OBJ_DIR)/%.o)
HEADLESS_OBJ := $(OBJ_DIR)/main.o
SDL_OBJ := $(OBJ_DIR)/main_sdl.o

HEADLESS_EXE := $(BIN_DIR)/emu$(EXE)
SDL_EXE := $(BIN_DIR)/emu_sdl$(EXE)

DEPS := $(CORE_OBJ:.o=.d) $(HEADLESS_OBJ:.o=.d) $(SDL_OBJ:.o=.d)

.PHONY: all headless sdl run run-sdl clean

all: headless sdl

headless: $(HEADLESS_EXE)

sdl: $(SDL_EXE)

$(HEADLESS_EXE): $(CORE_OBJ) $(HEADLESS_OBJ) | $(BIN_DIR)
	$(CC) $^ -o $@ $(LDLIBS)

$(SDL_EXE): $(CORE_OBJ) $(SDL_OBJ) | $(BIN_DIR)
	$(CC) $^ -o $@ $(SDL_LDLIBS) $(LDLIBS)

# Only the SDL frontend needs the SDL headers.
$(SDL_OBJ): CFLAGS += $(SDL_CFLAGS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

run: headless
	./$(HEADLESS_EXE) $(ROM)

run-sdl: sdl
	./$(SDL_EXE) $(ROM)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS)