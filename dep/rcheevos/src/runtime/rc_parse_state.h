#ifndef RC_RUNTIME_PARSE_STATE_H
#define RC_RUNTIME_PARSE_STATE_H

#include "rc_modified_memref.h" /* rc_memrefs_t */
#include "rc_operand.h"

#include "rc_util.h" /* rc_buffer_t */

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_scratch_t;        /* rc_alloc.h */
struct rc_scratch_string_t; /* rc_alloc.h */
struct rc_value_t;          /* rc_value.h */

typedef struct rc_parse_state_t {
  /* The amount of memory needed so far. Negative values are error codes. */
  int32_t offset;

  /* The amount of memory available in `buffer`. */
  uint32_t buffer_size;

  /* The allocated memory to write to. If null, we're just calculating how much memory will be needed. */
  void* buffer;

  /* Additional memory for temporary parsing data. */
  rc_buffer_t scratch_buffer;

  /* Collection of singletons for parsing when `buffer` is not available. */
  struct rc_scratch_t* scratch;

  /* Binary tree pointing at allocated strings so duplicate values can be shared. */
  struct rc_scratch_string_t* strings;

  /* The collection of memrefs allocated by the parser. */
  rc_memrefs_t* memrefs;

  /* A collection of memrefs available to the parser. */
  rc_memrefs_t* existing_memrefs;

  /* Pointer to the head of the variables list. */
  struct rc_value_t** variables;

  /* The current AddSource accumulator value. */
  rc_operand_t addsource_parent;

  /* The current AddAddress value. */
  rc_operand_t indirect_parent;

  /* The current Remember value. */
  rc_operand_t remember;

  /* The number of lines processed. Only applies to rich presence. */
  int lines_read;

  /* The measured target. */
  uint32_t measured_target;

  /* The combining operator for the AddSource accumulator value (RC_OPERATOR_*) */
  uint8_t addsource_oper;

  /* Non-zero if a value is being parsed. */
  uint8_t is_value;

  /* Non-zero if any parsed condition has a non-zero required hit target. */
  uint8_t has_required_hits;

  /* Non-zero if the measured target should be reported as a percentage. */
  uint8_t measured_as_percent;

  /* Non-zero to not fail if validation errors are encountered (like multiple measured targets). */
  uint8_t ignore_non_parse_errors;
} rc_parse_state_t;

typedef struct rc_preparse_state_t {
  /* Parse state */
  rc_parse_state_t parse;

  /* A memrefs collection for new memrefs needed by the object being parsed. */
  rc_memrefs_t memrefs;
} rc_preparse_state_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Initializes an `rc_parse_state_t` object.
 */
void rc_init_parse_state(struct rc_parse_state_t* parse, void* buffer, size_t buffer_size);

/**
 * Initializes the `memrefs` collection and assigns it to the `rc_parse_state_t` object.
 */
void rc_init_parse_state_memrefs(struct rc_parse_state_t* parse, struct rc_memrefs_t* memrefs);

/**
 * Resets the parse state.
 */
void rc_reset_parse_state(struct rc_parse_state_t* parse, void* buffer, size_t buffer_size);

/**
 * Releases resources associated to the parse state.
 */
void rc_destroy_parse_state(struct rc_parse_state_t* parse);

/**
 * Initializes an `rc_preparse_state_t` object.
 */
void rc_init_preparse_state(struct rc_preparse_state_t* preparse);

/**
 * Releases resources associated to the preparse state.
 */
void rc_destroy_preparse_state(struct rc_preparse_state_t* preparse);

/**
 * Allocates exactly enough space in `memrefs` for the memrefs in `preparse.memrefs`.
 *
 * If `memrefs` is NULL, the additional space is reserved so the proper needed size is calculated.
 */
void rc_preparse_alloc_memrefs(struct rc_memrefs_t* memrefs, struct rc_preparse_state_t* preparse);

/**
 * Allocates enough space in `memrefs` for the memrefs in `preparse.memrefs`.
 *
 * Allocates power-of-two blocks, so there will likely be space for further memrefs to be added.
 */
void rc_preparse_reserve_memrefs(struct rc_preparse_state_t* preparse, struct rc_memrefs_t* memrefs);

/**
 * Copies memrefs from `memrefs` into `parse->memrefs`. Existing items will not be duplicated.
 */
void rc_preparse_copy_memrefs(struct rc_parse_state_t* parse, const struct rc_memrefs_t* memrefs);

RC_END_C_DECLS

#endif /* RC_RUNTIME_PARSE_STATE_H */
