#ifndef RC_RUNTIME_MEMREF_H
#define RC_RUNTIME_MEMREF_H

#include "rc_export.h"

#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

enum {
  RC_MEMSIZE_8_BITS,
  RC_MEMSIZE_16_BITS,
  RC_MEMSIZE_24_BITS,
  RC_MEMSIZE_32_BITS,
  RC_MEMSIZE_LOW,
  RC_MEMSIZE_HIGH,
  RC_MEMSIZE_BIT_0,
  RC_MEMSIZE_BIT_1,
  RC_MEMSIZE_BIT_2,
  RC_MEMSIZE_BIT_3,
  RC_MEMSIZE_BIT_4,
  RC_MEMSIZE_BIT_5,
  RC_MEMSIZE_BIT_6,
  RC_MEMSIZE_BIT_7,
  RC_MEMSIZE_BITCOUNT,
  RC_MEMSIZE_16_BITS_BE,
  RC_MEMSIZE_24_BITS_BE,
  RC_MEMSIZE_32_BITS_BE,
  RC_MEMSIZE_FLOAT,
  RC_MEMSIZE_MBF32,
  RC_MEMSIZE_MBF32_LE,
  RC_MEMSIZE_FLOAT_BE,
  RC_MEMSIZE_DOUBLE32,
  RC_MEMSIZE_DOUBLE32_BE,
  RC_MEMSIZE_VARIABLE
};

enum {
  RC_MEMREF_TYPE_MEMREF,                 /* rc_memref_t */
  RC_MEMREF_TYPE_MODIFIED_MEMREF,        /* rc_modified_memref_t */
  RC_MEMREF_TYPE_VALUE                   /* rc_value_t */
};

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_parse_state_t; /* rc_parse_state.h */
struct rc_typed_value_t; /* rc_typed_value.h */

typedef struct rc_memref_value_t {
  /* The current value of this memory reference. */
  uint32_t value;
  /* The last differing value of this memory reference. */
  uint32_t prior;

  /* The size of the value. (RC_MEMSIZE_*) */
  uint8_t size;
  /* True if the value changed this frame. */
  uint8_t changed;
  /* The value type of the value. (RC_VALUE_TYPE_*) */
  uint8_t type;
  /* The type of memref (RC_MEMREF_TYPE_*) */
  uint8_t memref_type;
} rc_memref_value_t;

typedef struct rc_memref_t {
  /* The current value at the specified memory address. */
  struct rc_memref_value_t value;

  /* The memory address of this variable. */
  uint32_t address;
} rc_memref_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Determines if the provided size represents floating point data.
 */
int rc_memsize_is_float(uint8_t size);

/**
 * Extracts the `address` and `size` from a serialized memref.
 */
int rc_parse_memref(const char** memaddr, uint8_t* size, uint32_t* address);

/**
 * Allocates (or returns an existing) `rc_memref_t` for the specified `address` and `size`.
 */
struct rc_memref_t* rc_alloc_memref(struct rc_parse_state_t* parse, uint32_t address, uint8_t size);

/**
 * Updates the current value of the provided memref to the provided value.
 */
void rc_update_memref_value(struct rc_memref_value_t* memref, uint32_t value);

/**
 * Gets the type (RC_MEMREF_TYPE_*) of the memref.
 */
int rc_get_memref_type(const struct rc_memref_t* memref);

/**
 * Gets the specified value from the provided memef.
 *
 * operand_type is:
 * - RC_OPERAND_ADDRESS for current value
 * - RC_OPERAND_DELTA for previous value
 * - RC_OPERAND_PRIOR for last differing value
 */
void rc_get_memref_value(struct rc_typed_value_t* value, const struct rc_memref_t* memref, int operand_type);

/**
 * Gets the number of bytes that need to be read to get data of the specified size.
 */
uint32_t rc_memref_bytes(uint8_t size);

/**
 * Gets a size containing the specified size (for transformable sizes).
 */
uint8_t rc_memref_shared_size(uint8_t size);

/**
 * Gets a mask of which bits of a shared size apply to the specified size.
 */
uint32_t rc_memref_mask(uint8_t size);

/**
 * Converts a shared-size value read by `rc_get_memref_value` into a value appropriate for the specified size.
 */
void rc_transform_memref_value(struct rc_typed_value_t* value, uint8_t size);

RC_END_C_DECLS

#endif /* RC_RUNTIME_MEMREF_H */
