#include "bus.h"
#include "cart.h"
#include "timer.h"
#include "serial.h"
#include "ppu.h"
#include "joypad.h"
#include "dma.h"
#include "apu.h"
#include <stdlib.h>
#include <assert.h>

struct Bus {
  Cart* cart; //Refer to Mem Map
  Timer* timer; //Owns 0xFF04 - 0xFF07; ticked by sys_tick.
  Serial* serial; //Owns 0xFF01 - 0xFF02.
  Ppu* ppu; //Owns 0x8000-0x9FFF (VRAM), 0xFE00-0xFE9F (OAM), 0xFF40-0xFF4B except 0xFF46.
  Joypad* joypad; //Owns 0xFF00.
  Dma* dma; //Owns 0xFF46. Ticked once per M-cycle from sys_tick.
  Apu* apu; //Owns 0xFF10-0xFF3F. Basic square-wave audio.
  uint8_t wram[0x2000]; //8KB: 0xC000 - 0xDFFF
  uint8_t hram[0x7F]; //127 Bytes: 0xFF80 - 0xFFFE
  uint8_t if_reg; //Addr 0xFF0F
  uint8_t ie; //Addr 0xFFFF
  uint64_t t_cycles;  //total T-cycles elasped
};

static uint8_t dispatch_read(const Bus* b, uint16_t addr);
static void dispatch_write(Bus* b, uint16_t addr, uint8_t value);

Bus *bus_create(Cart* cart) {
  assert(cart != NULL); //Assert before malloc'ing

  Bus *bus = calloc(1, sizeof(Bus));
  if(bus == NULL) return NULL;  //FAIL

  bus->cart = cart;

  // Timer creation can fail; if it does, unwind cleanly.
  bus->timer = timer_create(bus);
  if (bus->timer == NULL) {
    free(bus);
    return NULL;
  }
  timer_reset(bus->timer);

  bus->serial = serial_create();
  if (bus->serial == NULL) {
    timer_destroy(bus->timer);
    free(bus);
    return NULL;
  }
  serial_reset(bus->serial);

  bus->ppu = ppu_create(bus);
  if (bus->ppu == NULL) {
    serial_destroy(bus->serial);
    timer_destroy(bus->timer);
    free(bus);
    return NULL;
  }
  ppu_reset(bus->ppu);

  bus->joypad = joypad_create(bus);
  if (bus->joypad == NULL) {
    ppu_destroy(bus->ppu);
    serial_destroy(bus->serial);
    timer_destroy(bus->timer);
    free(bus);
    return NULL;
  }
  joypad_reset(bus->joypad);

  bus->apu = apu_create();
  if (bus->apu == NULL) {
    apu_destroy(bus->apu);
    joypad_destroy(bus->joypad);
    ppu_destroy(bus->ppu);
    serial_destroy(bus->serial);
    timer_destroy(bus->timer);
    free(bus);
    return NULL;
  }
  apu_reset(bus->apu);

  bus->dma = dma_create(bus);
  if (bus->dma == NULL) {
    apu_destroy(bus->apu);
    joypad_destroy(bus->joypad);
    ppu_destroy(bus->ppu);
    serial_destroy(bus->serial);
    timer_destroy(bus->timer);
    free(bus);
    return NULL;
  }
  dma_reset(bus->dma);

  return bus;
}

void bus_destroy(Bus* b) {
  if (b == NULL) return;
  dma_destroy(b->dma);
  apu_destroy(b->apu);
  joypad_destroy(b->joypad);
  ppu_destroy(b->ppu);
  serial_destroy(b->serial);
  timer_destroy(b->timer);
  free(b);
}

uint8_t bus_read(Bus* b, uint16_t addr) {
  sys_tick(b, 1);
  // During an OAM DMA transfer the CPU can only see HRAM (and IE).
  // Reads of any other address return 0xFF.
  if (dma_active(b->dma) && !(addr >= 0xFF80 && addr <= 0xFFFE) && addr != 0xFFFF) {
    return 0xFF;
  }
  return dispatch_read(b, addr);
}

void bus_write(Bus* b, uint16_t addr, uint8_t value) {
  sys_tick(b, 1);
  // During an OAM DMA transfer, writes outside HRAM/IE are dropped --
  // EXCEPT the DMA register itself (0xFF46), which must remain writable
  // so a game can restart DMA mid-transfer.
  if (dma_active(b->dma)
      && !(addr >= 0xFF80 && addr <= 0xFFFE)
      && addr != 0xFFFF
      && addr != 0xFF46) {
    return;
  }
  dispatch_write(b, addr, value);
}

//No-tick funcs for internal use
uint8_t bus_peek(const Bus* b, uint16_t addr) {
  return dispatch_read(b, addr);
}

void bus_poke(Bus* b, uint16_t addr, uint8_t value) {
  dispatch_write(b, addr, value);
}

void sys_tick(Bus* b, int mcycles) {
  // Advance every clocked peripheral by the given number of M-cycles
  // (= 4 * mcycles T-cycles). Peripherals are ticked at T-cycle
  // granularity so their internal timing edges (timer-counter bits,
  // PPU mode transitions, etc.) fall on the correct cycle. DMA
  // operates at M-cycle granularity (1 byte per M-cycle), so it's
  // ticked once per outer M-cycle iteration.
  for (int m = 0; m < mcycles; m++) {
    for (int t = 0; t < 4; t++) {
      timer_tick_1t(b->timer);
      ppu_tick_1t(b->ppu);
      apu_tick_1t(b->apu);
    }
    dma_tick_1m(b->dma);
  }
  b->t_cycles += (uint64_t)(mcycles * 4);
}

void bus_request_interrupt(Bus* b, Interrupt which) {
  b->if_reg |= (uint8_t)(1u << (unsigned)which);
}

Timer* bus_timer(Bus* b) {
  return b->timer;
}

Ppu* bus_ppu(Bus* b) {
  return b->ppu;
}

Joypad* bus_joypad(Bus* b) {
  return b->joypad;
}

Dma* bus_dma(Bus* b) {
  return b->dma;
}

Apu* bus_apu(Bus* b) {
  return b->apu;
}

uint64_t bus_total_t_cycles(const Bus* b) {
  return b->t_cycles;
}

static uint8_t dispatch_read(const Bus* b, uint16_t addr) {
  //TODO: Consider just raising an error when hitting stubbed region in memory
  if (addr == 0xFFFF) {
    return b->ie;
  } else if (addr == 0xFF0F) {
    // IF: upper 3 bits read as 1 on real hardware (only bottom 5 are used).
    return (uint8_t)(b->if_reg | 0xE0);
  } else if (addr <= 0x7FFF) {
    return cart_read(b->cart, addr);  //ROM
  } else if (addr <= 0x9FFF) {
    return ppu_read_vram(b->ppu, addr);
  } else if (addr <= 0xBFFF) {
    return cart_read(b->cart, addr);  //Cart RAM
  } else if (addr <= 0xDFFF) {
    return b->wram[addr - 0xC000];  //WRAM
  } else if (addr <= 0xFDFF) {
    return b->wram[addr - 0xE000];  //ERAM
  } else if (addr <= 0xFE9F) {
    return ppu_read_oam(b->ppu, addr);
  } else if (addr <= 0xFEFF) {
    return 0xFF;                     //Unusable
  } else if (addr == 0xFF00) {
    return joypad_read(b->joypad, addr);
  } else if (addr == 0xFF01 || addr == 0xFF02) {
    return serial_read(b->serial, addr);
  } else if (addr >= 0xFF04 && addr <= 0xFF07) {
    return timer_read(b->timer, addr);
  } else if (addr >= 0xFF10 && addr <= 0xFF3F) {
    return apu_read(b->apu, addr);
  } else if ((addr >= 0xFF40 && addr <= 0xFF45) ||
             (addr >= 0xFF47 && addr <= 0xFF4B)) {
    // PPU registers (0xFF46 is OAM DMA, separate module).
    return ppu_read_reg(b->ppu, addr);
  } else if (addr == 0xFF46) {
    return dma_read(b->dma);
  } else if (addr <= 0xFF7F) {
    //TODO: read from I/O
    return 0xFF;                     //I/O
  } else if (addr <= 0xFFFE) {
    return b->hram[addr - 0xFF80];   //HRAM
  } else {
    return 0xFF; //Unmapped
  }
}

static void dispatch_write(Bus* b, uint16_t addr, uint8_t value) {
  if (addr == 0xFFFF) {
    b->ie = value;
  } else if (addr == 0xFF0F) {
    // IF: only the low 5 bits are meaningful. Upper bits ignored on write.
    b->if_reg = (uint8_t)(value & 0x1F);
  } else if (addr <= 0x7FFF) {
    cart_write(b->cart, addr, value);  //ROM
  } else if (addr <= 0x9FFF) {
    ppu_write_vram(b->ppu, addr, value);
  } else if (addr <= 0xBFFF) {
    cart_write(b->cart, addr, value);  //Cart RAM
  } else if (addr <= 0xDFFF) {
    b->wram[addr - 0xC000] = value;  //WRAM
  } else if (addr <= 0xFDFF) {
    b->wram[addr - 0xE000] = value;  //ERAM
  } else if (addr <= 0xFE9F) {
    ppu_write_oam(b->ppu, addr, value);
  } else if (addr <= 0xFEFF) {
    return;                   //Unusable keep as return
  } else if (addr == 0xFF00) {
    joypad_write(b->joypad, addr, value);
  } else if (addr == 0xFF01 || addr == 0xFF02) {
    serial_write(b->serial, addr, value);
  } else if (addr >= 0xFF04 && addr <= 0xFF07) {
    timer_write(b->timer, addr, value);
  } else if (addr >= 0xFF10 && addr <= 0xFF3F) {
    apu_write(b->apu, addr, value);
  } else if ((addr >= 0xFF40 && addr <= 0xFF45) ||
             (addr >= 0xFF47 && addr <= 0xFF4B)) {
    ppu_write_reg(b->ppu, addr, value);
  } else if (addr == 0xFF46) {
    dma_write(b->dma, value);
  } else if (addr <= 0xFF7F) {
    return;                          //I/O, I really doubt we actually write to IO
  } else if (addr <= 0xFFFE) {
    b->hram[addr - 0xFF80] = value;   //HRAM
  } else {
    return; //Unmapped
  }
}