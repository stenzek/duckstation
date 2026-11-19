#ifndef RC_ALLOC_H
#define RC_ALLOC_H

#include "rc_export.h"

#include <stddef.h>
#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_parse_state_t; /* rc_parse_state.h */

typedef struct rc_scratch_t {
  /* placeholder so an offset of 0 can be provided to rc_alloc to avoid using a singleton */
  void* unused;

  /* list of classes that can be allocated by the RC_ALLOC_* helper macros */
  void* __rc_condition_t;
  void* __rc_condset_t;
  void* __rc_modified_memref_t;
  void* __rc_lboard_t;
  void* __rc_lboard_with_memrefs_t;
  void* __rc_memref_t;
  void* __rc_memref_list_t;
  void* __rc_memrefs_t;
  void* __rc_modified_memref_list_t;
  void* __rc_operand_t;
  void* __rc_richpresence_t;
  void* __rc_richpresence_display_t;
  void* __rc_richpresence_display_part_t;
  void* __rc_richpresence_lookup_t;
  void* __rc_richpresence_lookup_item_t;
  void* __rc_richpresence_with_memrefs_t;
  void* __rc_trigger_t;
  void* __rc_trigger_with_memrefs_t;
  void* __rc_value_t;
  void* __rc_value_with_memrefs_t;
} rc_scratch_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Allocates the specified amount of space in the `parse_state`.
 */
void* rc_alloc(struct rc_parse_state_t* parse_state, uint32_t size, uint32_t scratch_object_pointer_offset);

/**
 * Allocates the specified amount of space in the `parse_state` scratch buffer (will be discarded after parsing completes).
 */
void* rc_alloc_scratch(struct rc_parse_state_t* parse_state, uint32_t size, uint32_t scratch_object_pointer_offset);

/**
 * Allocates a string in the `parse_state`.
 */
const char* rc_alloc_str(struct rc_parse_state_t* parse, const char* text, size_t length);

/* get the offset of a field in a structure */
#define RC_OFFSETOF(type, field) (uint32_t)((uint8_t*)&((type*)NULL)->field - (uint8_t*)NULL)

/* allocate an instance of type */
#define RC_ALLOC(type, parse_state) ((type*)rc_alloc(parse_state, sizeof(type), RC_OFFSETOF(rc_scratch_t, __ ## type)))

/* allocate an instance of type that doesn't need to live after parsing completes */
#define RC_ALLOC_SCRATCH(type, parse_state) ((type*)rc_alloc_scratch(parse_state, sizeof(type), RC_OFFSETOF(rc_scratch_t, __ ## type)))

/* allocate an array of type instances */
#define RC_ALLOC_ARRAY(type, count, parse_state) ((type*)rc_alloc(parse_state, (count) * sizeof(type), RC_OFFSETOF(rc_scratch_t, __ ## type)))

/* allocate an array of type instances that don't need to live after parsing completes */
#define RC_ALLOC_ARRAY_SCRATCH(type, count, parse_state) ((type*)rc_alloc_scratch(parse_state, (count) * sizeof(type), RC_OFFSETOF(rc_scratch_t, __ ## type)))

/* allocate a container with an array of type instances */
#define RC_ALLOC_WITH_TRAILING(container_type, trailing_type, trailing_field, trailing_count, parse) ((container_type*)rc_alloc(parse, \
          RC_OFFSETOF(container_type, trailing_field) + trailing_count * sizeof(trailing_type), 0))

/* get the array of type instances allocated by RC_ALLOC_WITH_TRAILING */
#define RC_GET_TRAILING(container_pointer, container_type, trailing_type, trailing_field) (trailing_type*)(&((container_type*)(container_pointer))->trailing_field)

/* force alignment to 4 bytes on 32-bit systems, or 8 bytes on 64-bit systems */
#define RC_ALIGN(n) (((n) + (sizeof(void*)-1)) & ~(sizeof(void*)-1))

RC_END_C_DECLS

#endif /* RC_ALLOC_H */
