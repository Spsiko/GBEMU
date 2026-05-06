#ifndef CART_H
#define CART_H

#include <stdint.h>
#include <stddef.h>

typedef struct Cart Cart;

// Result codes for cart_load.
typedef enum {
  CART_OK                 =  0,
  CART_ERR_FILE_OPEN      = -1,
  CART_ERR_FILE_READ      = -2,
  CART_ERR_OOM            = -3,
  CART_ERR_ALREADY_LOADED = -4,
  CART_ERR_BAD_HEADER     = -5,
  CART_ERR_SAVE_OPEN      = -6,
  CART_ERR_SAVE_WRITE     = -7,
} CartResult;

//Lifecycle
Cart* cart_create(void);  //NULL on failure
void  cart_free(Cart* cart);

//ROM
int cart_load(Cart* cart, const char* path);

/**
 * @brief Load a ROM image from an in-memory buffer.
 *
 * The buffer is copied; the caller retains ownership of `data`.
 * Performs the same header validation as cart_load (size, MBC type,
 * checksum). Used by tests that synthesize ROMs without touching the
 * filesystem.
 *
 * Returns CART_OK on success, or one of the CART_ERR_* codes.
 */
int cart_load_from_buffer(Cart* cart, const uint8_t* data, size_t len);

void cart_eject(Cart* cart);
int cart_has_rom(const Cart* cart); //Non-zero for true; In here just in case

//Battery-backed RAM
int cart_save(Cart* cart);          //Writes dirty battery RAM, if any.

//Memory
uint8_t cart_read(const Cart* cart, uint16_t addr);
void cart_write(Cart* cart, uint16_t addr, uint8_t value);

//MISC
const char* cart_title(const Cart* cart);
size_t cart_rom_size(const Cart* cart);

#endif
