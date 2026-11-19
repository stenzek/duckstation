#ifndef RC_RUNTIME_MODIFIED_MEMREFS_H
#define RC_RUNTIME_MODIFIED_MEMREFS_H

#include "rc_export.h"
#include "rc_memref.h"
#include "rc_operand.h"

#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_eval_state_t;  /* rc_eval_state.h */
struct rc_parse_state_t; /* rc_parse_state.h */

typedef struct rc_modified_memref_t {
  /* For compatibility with rc_operand_t.value.memref */
  rc_memref_t memref;

  /* The parent memref this memref is derived from (type will always be a memref type) */
  rc_operand_t parent;

  /* The modifier to apply to the parent. */
  rc_operand_t modifier;

  /* How to apply the modifier to the parent. (RC_OPERATOR_*) */
  uint8_t modifier_type;

  /* The number of parents this memref has. */
  uint16_t depth;
} rc_modified_memref_t;

typedef struct rc_memref_list_t {
  /* The actual memref items. */
  rc_memref_t* items;

  /* Pointer to more memref items if this list becomes full. */
  struct rc_memref_list_t* next;

  /* The current number of items in the list. */
  uint16_t count;

  /* The maximum number of items in the list. */
  uint16_t capacity;

  /* Non-zero if this list was allocated and needs to be free'd when `rc_memrefs_destroy` is called. */
  uint8_t allocated;
} rc_memref_list_t;

typedef struct rc_modified_memref_list_t {
  /* The actual modified memref items. */
  rc_modified_memref_t* items;

  /* Pointer to more modified memref items if this list becomes full. */
  struct rc_modified_memref_list_t* next;

  /* The current number of items in the list. */
  uint16_t count;

  /* The maximum number of items in the list. */
  uint16_t capacity;

  /* Non-zero if this list was allocated and needs to be free'd when `rc_memrefs_destroy` is called. */
  uint8_t allocated;
} rc_modified_memref_list_t;

typedef struct rc_memrefs_t {
  /* The list of direct memory references. */
  rc_memref_list_t memrefs;

  /* The list of derived memory references. */
  rc_modified_memref_list_t modified_memrefs;
} rc_memrefs_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Allocates (or returns an existing) `rc_memref_t` for the specified `address` and `size`.
 */
struct rc_modified_memref_t* rc_alloc_modified_memref(struct rc_parse_state_t* parse, uint8_t size, const struct rc_operand_t* parent,
                                                      uint8_t modifier_type, const rc_operand_t* modifier);

/**
 * Updates the current value of all memrefs.
 */
void rc_update_memref_values(struct rc_memrefs_t* memrefs, struct rc_eval_state_t* eval_state);

/**
 * Calculates the value of a modified_memref.
 */
uint32_t rc_get_modified_memref_value(const rc_modified_memref_t* memref, struct rc_eval_state_t* eval_state);

/**
 * Initializes a memrefs collection.
 */
void rc_memrefs_init(struct rc_memrefs_t* memrefs);

/**
 * Releases resources associated to a memrefs collection.
 */
void rc_memrefs_destroy(struct rc_memrefs_t* memrefs);

/**
 * Returns the number of direct memory references in the collection.
 */
uint32_t rc_memrefs_count_memrefs(const struct rc_memrefs_t* memrefs);

/**
 * Returns the number of derived memory references in the collection.
 */
uint32_t rc_memrefs_count_modified_memrefs(const struct rc_memrefs_t* memrefs);

RC_END_C_DECLS

#endif /* RC_RUNTIME_MODIFIED_MEMREFS_H */
