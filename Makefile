CC := gcc

CFLAGS := -std=c11 -Wall -Wextra -O2 -Iinclude
DEPFLAGS := -MMD -MP

SDL_CFLAGS := -I../x86_64-w64-mingw32/include
SDL_LDFLAGS := -L../x86_64-w64-mingw32/lib -lmingw32 -lSDL2main -lSDL2

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

HEADLESS_SRC := src/main.c
SDL_SRC := src/main_sdl.c

CORE_OBJ := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(CORE_SRC))
HEADLESS_OBJ := $(OBJ_DIR)/main.o
SDL_OBJ := $(OBJ_DIR)/main_sdl.sdl.o

HEADLESS_EXE := $(BIN_DIR)/emu.exe
SDL_EXE := $(BIN_DIR)/emu_sdl.exe

DEPS := $(CORE_OBJ:.o=.d) $(HEADLESS_OBJ:.o=.d) $(SDL_OBJ:.o=.d)

.PHONY: all headless sdl dirs run run-sdl clean distclean

all: headless sdl

headless: $(HEADLESS_EXE)

sdl: $(SDL_EXE)

dirs:
	mkdir -p $(OBJ_DIR) $(BIN_DIR)

$(HEADLESS_EXE): $(CORE_OBJ) $(HEADLESS_OBJ) | dirs
	$(CC) $(CFLAGS) $^ -o $@

$(SDL_EXE): $(CORE_OBJ) $(SDL_OBJ) | dirs
	$(CC) $(CFLAGS) $^ -o $@ $(SDL_LDFLAGS)

$(OBJ_DIR)/main.o: src/main.c | dirs
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ_DIR)/main_sdl.sdl.o: src/main_sdl.c | dirs
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.c | dirs
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

run: headless
	./$(HEADLESS_EXE) $(ROM)

run-sdl: sdl
	./$(SDL_EXE) $(ROM)

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -f $(HEADLESS_EXE) $(SDL_EXE)

-include $(DEPS)