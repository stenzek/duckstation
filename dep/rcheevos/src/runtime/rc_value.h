#ifndef RC_RUNTIME_VALUE_H
#define RC_RUNTIME_VALUE_H

#include "rc_memref.h"

#include <stddef.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

#define RC_VALUE_MAX_NAME_LENGTH 15

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_condset_t;     /* rc_condset.h */
struct rc_eval_state_t;  /* rc_eval_state.h */
struct rc_memrefs_t;     /* rc_modified_memrefs.h */
struct rc_parse_state_t; /* rc_parse_state.h */
struct rc_typed_value_t; /* rc_typed_value.h */

typedef struct rc_value_t {
  /* The current value of the variable. */
  struct rc_memref_value_t value;

  /* True if the value has its own rc_memrefs_t */
  uint8_t has_memrefs;

  /* The list of possible values (traverse next chain, pick max). */
  struct rc_condset_t* conditions;

  /* The name of the variable. */
  const char* name;

  /* The next variable in the chain. */
  struct rc_value_t* next;
} rc_value_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Returns non-zero if the value is a count of the number of times some condition is true instead of a value from memory.
 */
int rc_value_from_hits(struct rc_value_t* self);

/**
 * Allocates an `rc_value_t` from the provided serialized value.
 */
struct rc_value_t* rc_alloc_variable(const char* memaddr, size_t memaddr_len, struct rc_parse_state_t* parse);

/**
 * Gets the memrefs owned by the value.
 *
 * Not valid if the value was parsed using `rc_parse_value_internal`.
 */
struct rc_memrefs_t* rc_value_get_memrefs(struct rc_value_t* self);

/**
 * Allocates space for a value that owns its own memrefs.
 */
struct rc_value_t* rc_alloc_value_with_memrefs(struct rc_parse_state_t* parse);

/**
 * Determines how much memory is needed to store the deserialized value.
 */
int rc_value_size(const char* memaddr);

/**
 * Deserializes a serialized value using a preallocated `buffer`.
 */
struct rc_value_t* rc_parse_value(void* buffer, const char* memaddr, void* unused_L, int unused_funcs_idx);

/**
 * Extracts an `rc_value_t` from a serialized value.
 */
void rc_parse_value_internal(struct rc_value_t* self, const char** memaddr, struct rc_parse_state_t* parse);

/**
 * Processes a value and returns the new value.
 */
int32_t rc_evaluate_value(struct rc_value_t* self, struct rc_eval_state_t* eval_state);

/**
 * Processed a value into `result` and returns non-zero if `result` was populated.
 */
int rc_evaluate_value_typed(struct rc_value_t* self, struct rc_typed_value_t* result, struct rc_eval_state_t* eval_state);

/**
 * Extracts a variable name from the input stream into `buffer`.
 *
 * Returns non-zero if `buffer` was populated.
 */
int rc_value_get_variable_name(char buffer[], size_t buffer_size, const char** memaddr);

/**
 * Resets the captured hit count for every condition in the value.
 */
void rc_reset_value(struct rc_value_t* self);

/**
 * Resets the captured hit count for every condition in every value of the linked list starting at `values`.
 */
void rc_reset_values(struct rc_value_t* values);

/**
 * Counts the number of items in the linked list starting at `values`.
 */
uint32_t rc_count_values(const struct rc_value_t* values);

/**
 * Processes the values in the linked list starting at `values`.
 */
void rc_update_values(struct rc_value_t* values, struct rc_eval_state_t* eval_state);

RC_END_C_DECLS

#endif /* RC_RUNTIME_VALUE_H */
