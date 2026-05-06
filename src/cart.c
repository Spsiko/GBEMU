#include "cart.h"
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

typedef enum {
  MBC_EMPTY,
  MBC_NONE,
  MBC_1,
  MBC_2,
  MBC_3,
  MBC_5,
  MBC_UNSUPPORTED,
} MbcKind;

/**
 * @brief MBC1 register state.
 *
 * The four control registers, the mode flag, and a pre-computed mask
 * derived from the actual ROM size. Bank selection logic uses these
 * fields exclusively; cart_read does not need to know the kind any
 * deeper than "this is MBC1, dispatch to its handler."
 */
typedef struct {
  uint8_t bank_lo;        // 5 bits (writes to 0x2000-0x3FFF). 0 is silently bumped to 1.
  uint8_t bank_hi;        // 2 bits (writes to 0x4000-0x5FFF). Doubles as RAM bank in mode 1.
  uint8_t mode;           // 1 bit (writes to 0x6000-0x7FFF). 0 = simple, 1 = advanced.
  uint8_t ram_enable;     // 0 = disabled (reads 0xFF, writes ignored). Enabled when low nibble of write was 0xA.
  uint8_t rom_bank_mask;  // (rom_size / 16KB) - 1, e.g. 0x03 for 64KB, 0x7F for 2MB.
  uint8_t ram_bank_mask;  // (ram_size / 8KB) - 1, or 0 if no RAM.
} Mbc1;

typedef struct {
  uint8_t rom_bank;       // 4 bits; bank 0 is bumped to 1.
  uint8_t ram_enable;     // MBC2 has 512 x 4-bit internal RAM.
} Mbc2;

typedef struct {
  uint8_t rom_bank;       // 7 bits; bank 0 is bumped to 1.
  uint8_t ram_bank;       // 0-3 select RAM; 0x08-0x0C select RTC registers.
  uint8_t ram_enable;
  uint8_t has_timer;
  uint8_t rtc_warning_printed;
  uint8_t rom_bank_mask;
  uint8_t ram_bank_mask;
} Mbc3;

typedef struct {
  uint16_t rom_bank;      // 9 bits.
  uint8_t ram_bank;       // 0-15, or 0-7 for rumble carts when bit 3 is rumble.
  uint8_t ram_enable;
  uint8_t has_rumble;
  uint16_t rom_bank_mask;
  uint8_t ram_bank_mask;
} Mbc5;

struct Cart {
  uint8_t* rom;
  size_t rom_size;
  uint8_t* ram;       // cart RAM (battery or otherwise); NULL if none.
  size_t ram_size;
  char title[17];     //0x0134
  uint8_t mbc_type_raw; //raw byte from header offset 0x0147
  MbcKind mbc_kind;
  Mbc1 mbc1;
  Mbc2 mbc2;
  Mbc3 mbc3;
  Mbc5 mbc5;
  uint8_t has_battery;
  uint8_t ram_dirty;
  char save_path[512];
};

static MbcKind decode_mbc_kind(uint8_t raw);
static uint8_t compute_header_checksum(const uint8_t* rom);
static size_t  decode_ram_size(uint8_t code);
static int     finalize_loaded_rom(Cart* cart, uint8_t* buffer, size_t sz, const char* path);
static uint8_t cart_type_has_battery(uint8_t raw);
static uint8_t cart_type_has_timer(uint8_t raw);
static uint8_t cart_type_has_rumble(uint8_t raw);
static void    make_save_path(const char* rom_path, char* out, size_t out_size);
static int     cart_load_save(Cart* cart);
static uint8_t read_ram_bank0(const Cart* c, uint16_t addr);
static void    write_ram_bank0(Cart* c, uint16_t addr, uint8_t value);

Cart* cart_create(void) {
  return calloc(1, sizeof(Cart));
}

void cart_free(Cart* cart) {
  if (cart == NULL) return;
  cart_save(cart);
  free(cart->rom);
  free(cart->ram);
  free(cart);
}

//ROM
int cart_load(Cart* cart, const char* path) {
  if (cart->rom != NULL) return CART_ERR_ALREADY_LOADED;

  FILE* fp = fopen(path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open ROM at: %s\n", path);
    return CART_ERR_FILE_OPEN;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    fprintf(stderr, "Failed to seek ROM file: %s\n", path);
    return CART_ERR_FILE_READ;
  }

  long sz_raw = ftell(fp);
  if (sz_raw < 0) {
    fclose(fp);
    fprintf(stderr, "ftell failed on: %s\n", path);
    return CART_ERR_FILE_READ;
  }
  if (sz_raw < 0x8000 || sz_raw > 0x800000) {
    fclose(fp);
    fprintf(stderr, "Invalid ROM size: %ld bytes in %s\n", sz_raw, path);
    return CART_ERR_FILE_READ;
  }
  size_t sz = (size_t)sz_raw;

  rewind(fp);
  uint8_t* buffer = malloc(sz);
  if (buffer == NULL) {
    fclose(fp);
    fprintf(stderr, "Failed to allocate ROM buffer for: %s\n", path);
    return CART_ERR_OOM;
  }

  size_t n = fread(buffer, 1, sz, fp);
  fclose(fp);
  if (n != sz) {
    fprintf(stderr, "Short read: got %zu / %zu bytes from %s\n", n, sz, path);
    free(buffer);
    return CART_ERR_FILE_READ;
  }

  int rc = finalize_loaded_rom(cart, buffer, sz, path);
  if (rc != CART_OK) {
    free(buffer);
    return rc;
  }
  return CART_OK;
}

int cart_load_from_buffer(Cart* cart, const uint8_t* data, size_t len) {
  if (cart->rom != NULL) return CART_ERR_ALREADY_LOADED;
  if (len < 0x8000 || len > 0x800000) {
    return CART_ERR_BAD_HEADER;
  }
  uint8_t* buffer = malloc(len);
  if (buffer == NULL) return CART_ERR_OOM;
  memcpy(buffer, data, len);

  int rc = finalize_loaded_rom(cart, buffer, len, NULL);
  if (rc != CART_OK) {
    free(buffer);
    return rc;
  }
  return CART_OK;
}

/**
 * @brief Validate a loaded ROM image and finalize the cart state.
 *
 * Takes ownership of `buffer` on success (cart->rom = buffer).
 * On failure returns a CART_ERR_* code; the caller is responsible
 * for freeing the buffer.
 */
static int finalize_loaded_rom(Cart* cart, uint8_t* buffer, size_t sz, const char* path) {
  // MBC type
  uint8_t mbc_raw = buffer[0x0147];
  MbcKind kind = decode_mbc_kind(mbc_raw);
  if (kind == MBC_UNSUPPORTED) {
    fprintf(stderr, "Unsupported MBC type 0x%02X\n", mbc_raw);
    return CART_ERR_BAD_HEADER;
  }

  // ROM size (must match file size)
  uint8_t rom_size_code = buffer[0x0148];
  size_t header_rom_size = rom_size_code > 0x08 ? 0 : (size_t)0x8000 << rom_size_code;
  if (header_rom_size == 0) {
    fprintf(stderr, "Invalid ROM size code 0x%02X\n", rom_size_code);
    return CART_ERR_BAD_HEADER;
  }
  if (header_rom_size != sz) {
    fprintf(stderr, "ROM size mismatch: header says %zu, buffer is %zu\n",
            header_rom_size, sz);
    return CART_ERR_BAD_HEADER;
  }

  // Header checksum
  uint8_t sum = compute_header_checksum(buffer);
  if (buffer[0x014D] != sum) {
    fprintf(stderr, "ROM checksum expected 0x%02X, got 0x%02X\n",
            buffer[0x014D], sum);
    return CART_ERR_BAD_HEADER;
  }

  // RAM size. MBC2 is the exception: it has internal 512 x 4-bit RAM.
  size_t ram_size = decode_ram_size(buffer[0x0149]);
  if (kind == MBC_2) {
    ram_size = 512;
  }

  uint8_t* ram = NULL;
  if (ram_size > 0) {
    ram = calloc(1, ram_size);
    if (ram == NULL) {
      fprintf(stderr, "Failed to allocate %zu bytes of cart RAM\n", ram_size);
      return CART_ERR_OOM;
    }
  }

  // Commit state.
  cart->mbc_type_raw = mbc_raw;
  cart->mbc_kind = kind;
  memcpy(cart->title, &buffer[0x0134], 16);
  cart->title[16] = '\0';
  cart->rom = buffer;
  cart->rom_size = sz;
  cart->ram = ram;
  cart->ram_size = ram_size;
  cart->has_battery = cart_type_has_battery(mbc_raw);
  cart->ram_dirty = 0;

  if (path != NULL) {
    make_save_path(path, cart->save_path, sizeof(cart->save_path));
  } else {
    cart->save_path[0] = '\0';
  }

  // Initialise MBC1 state. The defaults match power-on behaviour:
  // bank 1 selected at 0x4000, RAM disabled, mode 0.
  cart->mbc1.bank_lo = 1;
  cart->mbc1.bank_hi = 0;
  cart->mbc1.mode = 0;
  cart->mbc1.ram_enable = 0;
  // Mask is one less than the bank count, e.g. 64KB ROM = 4 banks -> mask 0x03.
  cart->mbc1.rom_bank_mask = (uint8_t)((sz / 0x4000) - 1);
  cart->mbc1.ram_bank_mask = ram_size > 0x2000
                             ? (uint8_t)((ram_size / 0x2000) - 1)
                             : 0;

  cart->mbc2.rom_bank = 1;
  cart->mbc2.ram_enable = 0;

  cart->mbc3.rom_bank = 1;
  cart->mbc3.ram_bank = 0;
  cart->mbc3.ram_enable = 0;
  cart->mbc3.has_timer = cart_type_has_timer(mbc_raw);
  cart->mbc3.rtc_warning_printed = 0;
  cart->mbc3.rom_bank_mask = (uint8_t)((sz / 0x4000) - 1);
  cart->mbc3.ram_bank_mask = ram_size > 0x2000
                             ? (uint8_t)((ram_size / 0x2000) - 1)
                             : 0;

  cart->mbc5.rom_bank = 1;
  cart->mbc5.ram_bank = 0;
  cart->mbc5.ram_enable = 0;
  cart->mbc5.has_rumble = cart_type_has_rumble(mbc_raw);
  cart->mbc5.rom_bank_mask = (uint16_t)((sz / 0x4000) - 1);
  cart->mbc5.ram_bank_mask = ram_size > 0x2000
                             ? (uint8_t)((ram_size / 0x2000) - 1)
                             : 0;

  if (cart->has_battery && cart->ram != NULL && cart->save_path[0] != '\0') {
    (void)cart_load_save(cart);
  }

  return CART_OK;
}

void cart_eject(Cart* cart) {
  cart_save(cart);
  free(cart->rom);
  cart->rom = NULL;
  cart->rom_size = 0;
  free(cart->ram);
  cart->ram = NULL;
  cart->ram_size = 0;
  cart->mbc_type_raw = 0x00;
  cart->mbc_kind = MBC_EMPTY;
  cart->has_battery = 0;
  cart->ram_dirty = 0;
  memset(cart->title, 0, sizeof(cart->title));
  memset(cart->save_path, 0, sizeof(cart->save_path));
  memset(&cart->mbc1, 0, sizeof(cart->mbc1));
  memset(&cart->mbc2, 0, sizeof(cart->mbc2));
  memset(&cart->mbc3, 0, sizeof(cart->mbc3));
  memset(&cart->mbc5, 0, sizeof(cart->mbc5));
}

int cart_has_rom(const Cart* cart) {
  return cart->rom != NULL;
}

int cart_save(Cart* cart) {
  if (cart == NULL) return CART_OK;
  if (!cart->has_battery || !cart->ram_dirty || cart->ram == NULL || cart->ram_size == 0) {
    return CART_OK;
  }
  if (cart->save_path[0] == '\0') {
    return CART_OK;
  }

  FILE* fp = fopen(cart->save_path, "wb");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open save file for writing: %s\n", cart->save_path);
    return CART_ERR_SAVE_OPEN;
  }

  size_t n = fwrite(cart->ram, 1, cart->ram_size, fp);
  fclose(fp);
  if (n != cart->ram_size) {
    fprintf(stderr, "Short save write: got %zu / %zu bytes to %s\n",
            n, cart->ram_size, cart->save_path);
    return CART_ERR_SAVE_WRITE;
  }

  cart->ram_dirty = 0;
  return CART_OK;
}

// =====================================================================
// MBC1 read/write
// =====================================================================
//
// MBC1 has four control registers (writes to ROM region) plus a mode
// flag, all packed into a single Mbc1 struct above. Reads from
// 0x4000-0x7FFF map to the "low + high" combined bank number; reads
// from 0x0000-0x3FFF normally come from bank 0, but in mode 1 with a
// nonzero high register they come from (high << 5) -- this is how
// 1MB+ ROMs reach banks 0x20, 0x40, 0x60.
//
// Bank numbers are masked against the actual ROM size, so an oversized
// register write wraps to a real bank rather than running off the end
// of the ROM. The 0->1 quirk on bank_lo is applied at read time, not
// write time, so the register value itself can be observed as 0 by
// tools that read it back -- but the address calculation always sees
// at least 1 in the low five bits.

static uint8_t mbc1_read(const Cart* c, uint16_t addr) {
  if (addr <= 0x3FFF) {
    // Lower bank. In mode 0 always bank 0; in mode 1 bank (hi << 5).
    uint32_t bank = (c->mbc1.mode == 0) ? 0 : ((uint32_t)c->mbc1.bank_hi << 5);
    bank &= c->mbc1.rom_bank_mask;
    uint32_t offset = bank * 0x4000u + addr;
    return c->rom[offset];
  }
  if (addr <= 0x7FFF) {
    // Upper bank. Combine hi << 5 with lo (0->1 fixup), then mask.
    uint32_t lo = c->mbc1.bank_lo == 0 ? 1 : c->mbc1.bank_lo;
    uint32_t bank = ((uint32_t)c->mbc1.bank_hi << 5) | lo;
    bank &= c->mbc1.rom_bank_mask;
    uint32_t offset = bank * 0x4000u + (addr - 0x4000);
    return c->rom[offset];
  }
  // 0xA000 - 0xBFFF: cart RAM.
  if (!c->mbc1.ram_enable || c->ram == NULL) return 0xFF;
  // In mode 0, RAM bank is always 0. In mode 1, hi register selects.
  uint32_t ram_bank = (c->mbc1.mode == 0) ? 0 : c->mbc1.bank_hi;
  ram_bank &= c->mbc1.ram_bank_mask;
  uint32_t offset = ram_bank * 0x2000u + (addr - 0xA000);
  if (offset >= c->ram_size) return 0xFF;
  return c->ram[offset];
}

static void mbc1_write(Cart* c, uint16_t addr, uint8_t value) {
  if (addr <= 0x1FFF) {
    // RAM enable. Anything with low nibble == 0xA enables; else disables.
    c->mbc1.ram_enable = ((value & 0x0F) == 0x0A) ? 1 : 0;
    return;
  }
  if (addr <= 0x3FFF) {
    // ROM bank low 5 bits.
    c->mbc1.bank_lo = (uint8_t)(value & 0x1F);
    return;
  }
  if (addr <= 0x5FFF) {
    // ROM bank high 2 bits / RAM bank.
    c->mbc1.bank_hi = (uint8_t)(value & 0x03);
    return;
  }
  if (addr <= 0x7FFF) {
    // Banking mode.
    c->mbc1.mode = (uint8_t)(value & 0x01);
    return;
  }
  // 0xA000 - 0xBFFF: RAM write.
  if (!c->mbc1.ram_enable || c->ram == NULL) return;
  uint32_t ram_bank = (c->mbc1.mode == 0) ? 0 : c->mbc1.bank_hi;
  ram_bank &= c->mbc1.ram_bank_mask;
  uint32_t offset = ram_bank * 0x2000u + (addr - 0xA000);
  if (offset < c->ram_size) {
    c->ram[offset] = value;
    c->ram_dirty = 1;
  }
}

// =====================================================================
// MBC2 read/write
// =====================================================================

static uint8_t mbc2_read(const Cart* c, uint16_t addr) {
  if (addr <= 0x3FFF) {
    return c->rom[addr];
  }
  if (addr <= 0x7FFF) {
    uint32_t bank = c->mbc2.rom_bank == 0 ? 1 : c->mbc2.rom_bank;
    bank &= (uint32_t)((c->rom_size / 0x4000) - 1);
    uint32_t offset = bank * 0x4000u + (addr - 0x4000);
    return c->rom[offset];
  }
  if (!c->mbc2.ram_enable || c->ram == NULL) return 0xFF;
  if (addr > 0xA1FF) return 0xFF;
  return (uint8_t)(0xF0 | (c->ram[addr - 0xA000] & 0x0F));
}

static void mbc2_write(Cart* c, uint16_t addr, uint8_t value) {
  if (addr <= 0x3FFF) {
    if ((addr & 0x0100) == 0) {
      c->mbc2.ram_enable = ((value & 0x0F) == 0x0A) ? 1 : 0;
    } else {
      c->mbc2.rom_bank = (uint8_t)(value & 0x0F);
      if (c->mbc2.rom_bank == 0) c->mbc2.rom_bank = 1;
    }
    return;
  }
  if (addr >= 0xA000 && addr <= 0xA1FF && c->mbc2.ram_enable && c->ram != NULL) {
    c->ram[addr - 0xA000] = value & 0x0F;
    c->ram_dirty = 1;
  }
}

// =====================================================================
// MBC3 read/write
// =====================================================================

static uint8_t mbc3_read(const Cart* c, uint16_t addr) {
  if (addr <= 0x3FFF) {
    return c->rom[addr];
  }
  if (addr <= 0x7FFF) {
    uint32_t bank = c->mbc3.rom_bank == 0 ? 1 : c->mbc3.rom_bank;
    bank &= c->mbc3.rom_bank_mask;
    uint32_t offset = bank * 0x4000u + (addr - 0x4000);
    return c->rom[offset];
  }
  if (!c->mbc3.ram_enable) return 0xFF;
  if (c->mbc3.ram_bank >= 0x08 && c->mbc3.ram_bank <= 0x0C) {
    // RTC registers are recognized but not fully implemented yet.
    return 0x00;
  }
  if (c->ram == NULL) return 0xFF;
  uint32_t ram_bank = c->mbc3.ram_bank & c->mbc3.ram_bank_mask;
  uint32_t offset = ram_bank * 0x2000u + (addr - 0xA000);
  if (offset >= c->ram_size) return 0xFF;
  return c->ram[offset];
}

static void mbc3_write(Cart* c, uint16_t addr, uint8_t value) {
  if (addr <= 0x1FFF) {
    c->mbc3.ram_enable = ((value & 0x0F) == 0x0A) ? 1 : 0;
    return;
  }
  if (addr <= 0x3FFF) {
    c->mbc3.rom_bank = (uint8_t)(value & 0x7F);
    if (c->mbc3.rom_bank == 0) c->mbc3.rom_bank = 1;
    return;
  }
  if (addr <= 0x5FFF) {
    c->mbc3.ram_bank = value;
    if (c->mbc3.has_timer && value >= 0x08 && value <= 0x0C && !c->mbc3.rtc_warning_printed) {
      fprintf(stderr, "Warning: MBC3 RTC register selected, RTC is stubbed\n");
      c->mbc3.rtc_warning_printed = 1;
    }
    return;
  }
  if (addr <= 0x7FFF) {
    // RTC latch command. Stubbed for now.
    return;
  }
  if (!c->mbc3.ram_enable) return;
  if (c->mbc3.ram_bank >= 0x08 && c->mbc3.ram_bank <= 0x0C) {
    // RTC write ignored for now.
    return;
  }
  if (c->ram == NULL) return;
  uint32_t ram_bank = c->mbc3.ram_bank & c->mbc3.ram_bank_mask;
  uint32_t offset = ram_bank * 0x2000u + (addr - 0xA000);
  if (offset < c->ram_size) {
    c->ram[offset] = value;
    c->ram_dirty = 1;
  }
}

// =====================================================================
// MBC5 read/write
// =====================================================================

static uint8_t mbc5_read(const Cart* c, uint16_t addr) {
  if (addr <= 0x3FFF) {
    return c->rom[addr];
  }
  if (addr <= 0x7FFF) {
    uint32_t bank = c->mbc5.rom_bank & c->mbc5.rom_bank_mask;
    uint32_t offset = bank * 0x4000u + (addr - 0x4000);
    return c->rom[offset];
  }
  if (!c->mbc5.ram_enable || c->ram == NULL) return 0xFF;
  uint32_t ram_bank = c->mbc5.ram_bank & c->mbc5.ram_bank_mask;
  uint32_t offset = ram_bank * 0x2000u + (addr - 0xA000);
  if (offset >= c->ram_size) return 0xFF;
  return c->ram[offset];
}

static void mbc5_write(Cart* c, uint16_t addr, uint8_t value) {
  if (addr <= 0x1FFF) {
    c->mbc5.ram_enable = ((value & 0x0F) == 0x0A) ? 1 : 0;
    return;
  }
  if (addr <= 0x2FFF) {
    c->mbc5.rom_bank = (uint16_t)((c->mbc5.rom_bank & 0x100) | value);
    return;
  }
  if (addr <= 0x3FFF) {
    c->mbc5.rom_bank = (uint16_t)((c->mbc5.rom_bank & 0x0FF) | ((value & 0x01) << 8));
    return;
  }
  if (addr <= 0x5FFF) {
    c->mbc5.ram_bank = c->mbc5.has_rumble ? (uint8_t)(value & 0x07)
                                           : (uint8_t)(value & 0x0F);
    return;
  }
  if (addr <= 0x7FFF) {
    return;
  }
  if (!c->mbc5.ram_enable || c->ram == NULL) return;
  uint32_t ram_bank = c->mbc5.ram_bank & c->mbc5.ram_bank_mask;
  uint32_t offset = ram_bank * 0x2000u + (addr - 0xA000);
  if (offset < c->ram_size) {
    c->ram[offset] = value;
    c->ram_dirty = 1;
  }
}

//Memory
uint8_t cart_read(const Cart* cart, uint16_t addr) {
  if (cart->rom == NULL) return 0xFF;  //This is accurate behaviour

  switch (cart->mbc_kind) {
    case MBC_NONE:
      if (addr <= 0x7FFF) return cart->rom[addr];
      if (addr >= 0xA000 && addr <= 0xBFFF) return read_ram_bank0(cart, addr);
      return 0xFF;
    case MBC_1:
      return mbc1_read(cart, addr);
    case MBC_2:
      return mbc2_read(cart, addr);
    case MBC_3:
      return mbc3_read(cart, addr);
    case MBC_5:
      return mbc5_read(cart, addr);
    case MBC_EMPTY:
    case MBC_UNSUPPORTED:
    default:
      return 0xFF;
  }
}

void cart_write(Cart* cart, uint16_t addr, uint8_t value) {
  if (cart->rom == NULL) return;

  switch (cart->mbc_kind) {
    case MBC_NONE:
      if (addr >= 0xA000 && addr <= 0xBFFF) write_ram_bank0(cart, addr, value);
      return;
    case MBC_1:
      mbc1_write(cart, addr, value);
      return;
    case MBC_2:
      mbc2_write(cart, addr, value);
      return;
    case MBC_3:
      mbc3_write(cart, addr, value);
      return;
    case MBC_5:
      mbc5_write(cart, addr, value);
      return;
    case MBC_EMPTY:
    case MBC_UNSUPPORTED:
    default:
      (void)addr; (void)value;
      return;
  }
}

//MISC
const char* cart_title(const Cart* cart) {
  return cart->title;
}

size_t cart_rom_size(const Cart* cart) {
  return cart->rom_size;
}

static MbcKind decode_mbc_kind(uint8_t raw) {
  switch (raw) {
    case 0x00: case 0x08: case 0x09:
      return MBC_NONE;
    case 0x01: case 0x02: case 0x03:
      return MBC_1;
    case 0x05: case 0x06:
      return MBC_2;
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13:
      return MBC_3;
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
      return MBC_5;
    default:
      return MBC_UNSUPPORTED;
  }
}

static uint8_t compute_header_checksum(const uint8_t* rom) {
  uint8_t sum = 0;
  for (uint16_t addr = 0x0134; addr <= 0x014C; addr++) {
    sum -= rom[addr] + 1;
  }
  return sum;
}

/**
 * @brief Decode the RAM size code at header offset 0x0149.
 *
 * Standard mappings:
 *   0x00 = 0 (no RAM)
 *   0x01 = 2 KB (rare; often unused -- emulators commonly treat as 8 KB)
 *   0x02 = 8 KB    (1 bank)
 *   0x03 = 32 KB   (4 banks)
 *   0x04 = 128 KB  (16 banks; only certain MBCs)
 *   0x05 = 64 KB   (8 banks)
 *
 * For MBC1 only sizes 0x00, 0x02, 0x03 are realistic; we accept the
 * full table for forward compatibility with MBC3/MBC5 carts.
 */
static size_t decode_ram_size(uint8_t code) {
  switch (code) {
    case 0x00: return 0;
    case 0x01: return 0x2000;    // treat 2 KB as one 8 KB bank
    case 0x02: return 0x2000;    // 8 KB
    case 0x03: return 0x8000;    // 32 KB
    case 0x04: return 0x20000;   // 128 KB
    case 0x05: return 0x10000;   // 64 KB
    default:   return 0;
  }
}

static uint8_t cart_type_has_battery(uint8_t raw) {
  switch (raw) {
    case 0x03: // MBC1 + RAM + BATTERY
    case 0x06: // MBC2 + BATTERY
    case 0x09: // ROM + RAM + BATTERY
    case 0x0F: // MBC3 + TIMER + BATTERY
    case 0x10: // MBC3 + TIMER + RAM + BATTERY
    case 0x13: // MBC3 + RAM + BATTERY
    case 0x1B: // MBC5 + RAM + BATTERY
    case 0x1E: // MBC5 + RUMBLE + RAM + BATTERY
      return 1;
    default:
      return 0;
  }
}

static uint8_t cart_type_has_timer(uint8_t raw) {
  return raw == 0x0F || raw == 0x10;
}

static uint8_t cart_type_has_rumble(uint8_t raw) {
  return raw == 0x1C || raw == 0x1D || raw == 0x1E;
}

static void make_save_path(const char* rom_path, char* out, size_t out_size) {
  if (out == NULL || out_size == 0) return;
  out[0] = '\0';
  if (rom_path == NULL) return;

  snprintf(out, out_size, "%s", rom_path);

  char* last_slash = strrchr(out, '/');
  char* last_backslash = strrchr(out, '\\');
  char* last_sep = last_slash;
  if (last_sep == NULL || (last_backslash != NULL && last_backslash > last_sep)) {
    last_sep = last_backslash;
  }

  char* dot = strrchr(out, '.');
  if (dot != NULL && (last_sep == NULL || dot > last_sep)) {
    snprintf(dot, out_size - (size_t)(dot - out), ".sav");
    return;
  }

  size_t len = strlen(out);
  if (len + 4 < out_size) {
    strcat(out, ".sav");
  }
}

static int cart_load_save(Cart* cart) {
  if (cart == NULL || cart->save_path[0] == '\0') return CART_OK;

  FILE* fp = fopen(cart->save_path, "rb");
  if (fp == NULL) {
    return CART_OK; // No save file yet is normal.
  }

  size_t n = fread(cart->ram, 1, cart->ram_size, fp);
  fclose(fp);

  if (n < cart->ram_size) {
    memset(cart->ram + n, 0, cart->ram_size - n);
  }

  cart->ram_dirty = 0;
  return CART_OK;
}

static uint8_t read_ram_bank0(const Cart* c, uint16_t addr) {
  if (c->ram == NULL) return 0xFF;
  uint32_t offset = addr - 0xA000;
  if (offset >= c->ram_size) return 0xFF;
  return c->ram[offset];
}

static void write_ram_bank0(Cart* c, uint16_t addr, uint8_t value) {
  if (c->ram == NULL) return;
  uint32_t offset = addr - 0xA000;
  if (offset < c->ram_size) {
    c->ram[offset] = value;
    c->ram_dirty = 1;
  }
}
