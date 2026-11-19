#include "rc_alloc.h"

#include "rc_error.h"
#include "rc_eval_state.h"
#include "rc_modified_memref.h"
#include "rc_operand.h"
#include "rc_operator.h"
#include "rc_parse_state.h"

#include <stdlib.h>
#include <string.h>

typedef struct rc_scratch_string_t rc_scratch_string_t;

struct rc_scratch_string_t {
  char* value;
  struct rc_scratch_string_t* left;
  struct rc_scratch_string_t* right;
};

void* rc_alloc_scratch(rc_parse_state_t* parse_state, uint32_t size, uint32_t scratch_object_pointer_offset)
{
  void* ptr;

  /* after an error the offset is an error code, adding sizes to it would hide the error */
  if (parse_state->offset < 0)
    return NULL;

  /* if we have a real buffer, then allocate the data there */
  if (parse_state->buffer)
    return rc_alloc(parse_state, size, scratch_object_pointer_offset);

  /* update how much space will be required in the real buffer */
  parse_state->offset = RC_ALIGN(parse_state->offset);
  parse_state->offset += size;

  /* find a scratch buffer to hold the temporary data */
  ptr = rc_buffer_alloc(&parse_state->scratch_buffer, size);
  if (!ptr)
    parse_state->offset = RC_OUT_OF_MEMORY;

  return ptr;
}

void* rc_alloc(rc_parse_state_t* parse_state, uint32_t size, uint32_t scratch_object_pointer_offset)
{
  void* ptr;

  /* after an error the offset is an error code, not a position in the buffer */
  if (parse_state->offset < 0)
    return NULL;

  parse_state->offset = RC_ALIGN(parse_state->offset);

  if (parse_state->buffer) {
    /* valid buffer, grab the next chunk */
    ptr = (void*)((uint8_t*)parse_state->buffer + parse_state->offset);

    parse_state->offset += size;
    if ((uint32_t)parse_state->offset > parse_state->buffer_size) {
      parse_state->offset = RC_INSUFFICIENT_BUFFER;
      return NULL;
    }

    return ptr;
  }

  if (scratch_object_pointer_offset) {
    void** scratch_object_pointer;

    /* make sure the scratch pointer array is available */
    if (!parse_state->scratch) {
      parse_state->scratch = (rc_scratch_t*)rc_buffer_alloc(&parse_state->scratch_buffer, sizeof(rc_scratch_t));
      if (!parse_state->scratch) {
        parse_state->offset = RC_OUT_OF_MEMORY;
        return NULL;
      }

      memset(parse_state->scratch, 0, sizeof(rc_scratch_t));
    }

    /* only allocate one instance of each object type (indentified by scratch_object_pointer_offset) */
    scratch_object_pointer = (void**)((uint8_t*)parse_state->scratch + scratch_object_pointer_offset);
    ptr = *scratch_object_pointer;

    if (ptr) {
      /* this object type's instance has been allocated, but we still need to tally space for the real data. */
      parse_state->offset += size;
    }
    else {
      /* this object type's instance hasn't been allocated yet. do so now. */
      ptr = *scratch_object_pointer = rc_alloc_scratch(parse_state, size, scratch_object_pointer_offset);
    }
  }
  else {
    ptr = rc_alloc_scratch(parse_state, size, scratch_object_pointer_offset);
  }

  return ptr;
}

const char* rc_alloc_str(rc_parse_state_t* parse, const char* text, size_t length)
{
  char* ptr;

  rc_scratch_string_t** next = &parse->strings;
  while (*next) {
    int diff = strncmp(text, (*next)->value, length);
    if (diff == 0) {
      diff = (*next)->value[length];
      if (diff == 0)
        return (*next)->value;
    }

    if (diff < 0)
      next = &(*next)->left;
    else
      next = &(*next)->right;
  }

  *next = (rc_scratch_string_t*)rc_buffer_alloc(&parse->scratch_buffer, sizeof(rc_scratch_string_t));
  ptr = (char*)rc_alloc(parse, (uint32_t)length + 1, 0);

  if (!ptr || !*next) {
    if (parse->offset >= 0)
      parse->offset = RC_OUT_OF_MEMORY;

    /* don't leave a node without a value in the tree */
    *next = NULL;
    return NULL;
  }

  memcpy(ptr, text, length);
  ptr[length] = '\0';

  (*next)->left = NULL;
  (*next)->right = NULL;
  (*next)->value = ptr;

  return ptr;
}

void rc_init_preparse_state(rc_preparse_state_t* preparse)
{
  rc_init_parse_state(&preparse->parse, NULL, 0);
  rc_init_parse_state_memrefs(&preparse->parse, &preparse->memrefs);
}

void rc_destroy_preparse_state(rc_preparse_state_t* preparse)
{
  rc_destroy_parse_state(&preparse->parse);
}

void rc_preparse_alloc_memrefs(rc_memrefs_t* memrefs, rc_preparse_state_t* preparse)
{
  const uint32_t num_memrefs = rc_memrefs_count_memrefs(&preparse->memrefs);
  const uint32_t num_modified_memrefs = rc_memrefs_count_modified_memrefs(&preparse->memrefs);

  if (preparse->parse.offset < 0)
    return;

  if (num_memrefs > 0xFFFF || num_modified_memrefs > 0xFFFF) {
    /* memref capacity is a 16-bit number. If we somehow have more than 64K unique memrefs, fail. */
    preparse->parse.offset = RC_INSUFFICIENT_BUFFER;
    return;
  }

  if (memrefs) {
    memset(memrefs, 0, sizeof(*memrefs));
    preparse->parse.memrefs = memrefs;
  }

  if (num_memrefs) {
    rc_memref_t* memref_items = RC_ALLOC_ARRAY(rc_memref_t, num_memrefs, &preparse->parse);

    if (memrefs) {
      memrefs->memrefs.capacity = (uint16_t)num_memrefs;
      memrefs->memrefs.items = memref_items;
    }
  }

  if (num_modified_memrefs) {
    rc_modified_memref_t* modified_memref_items =
      RC_ALLOC_ARRAY(rc_modified_memref_t, num_modified_memrefs, &preparse->parse);

    if (memrefs) {
      memrefs->modified_memrefs.capacity = (uint16_t)num_modified_memrefs;
      memrefs->modified_memrefs.items = modified_memref_items;
    }
  }

  /* when preparsing, this structure will be allocated at the end. when it's allocated earlier
   * in the buffer, it could be followed by something aligned at 8 bytes. force the offset to
   * an 8-byte boundary */
  if (!memrefs)
    preparse->parse.offset = RC_ALIGN(preparse->parse.offset);
}

static uint32_t rc_preparse_array_size(uint32_t needed, uint32_t minimum)
{
  while (minimum < needed)
    minimum <<= 1;

  /* Capacity is stored in a 16-bit value. If we've determined 16-bits is enough, return the maximum 16-bit value */
  if (minimum == 0x10000 && needed < 0x10000)
    minimum = 0xFFFF;

  return minimum;
}

void rc_preparse_reserve_memrefs(rc_preparse_state_t* preparse, rc_memrefs_t* memrefs)
{
  const uint32_t num_memrefs = rc_memrefs_count_memrefs(&preparse->memrefs);
  const uint32_t num_modified_memrefs = rc_memrefs_count_modified_memrefs(&preparse->memrefs);
  uint32_t available, new_size;

  if (preparse->parse.offset < 0)
    return;

  if (num_memrefs) {
    rc_memref_list_t* memref_list = &memrefs->memrefs;
    while (memref_list->count == memref_list->capacity) {
      if (!memref_list->next)
        break;

      memref_list = memref_list->next;
    }

    available = memref_list->capacity - memref_list->count;
    if (available < num_memrefs) {
      rc_memref_list_t* new_memref_list = (rc_memref_list_t*)calloc(1, sizeof(rc_memref_list_t));
      if (!new_memref_list) {
        preparse->parse.offset = RC_OUT_OF_MEMORY;
        return;
      }

      new_size = rc_preparse_array_size(num_memrefs - available, 16);
      if (new_size > 0xFFFF) {
        preparse->parse.offset = RC_INSUFFICIENT_BUFFER;
        return;
      }
      new_memref_list->capacity = (uint16_t)new_size;
      new_memref_list->items = (rc_memref_t*)malloc(new_size * sizeof(rc_memref_t));
      if (!new_memref_list->items) {
        free(new_memref_list);
        preparse->parse.offset = RC_OUT_OF_MEMORY;
        return;
      }

      new_memref_list->allocated = 1;

      /* link it at the end: a list in the middle may have room left after an
       * activation failed, and the lists after it must not be lost */
      while (memref_list->next)
        memref_list = memref_list->next;
      memref_list->next = new_memref_list;
    }
  }

  if (num_modified_memrefs) {
    rc_modified_memref_list_t* modified_memref_list = &memrefs->modified_memrefs;
    while (modified_memref_list->count == modified_memref_list->capacity) {
      if (!modified_memref_list->next)
        break;

      modified_memref_list = modified_memref_list->next;
    }

    available = modified_memref_list->capacity - modified_memref_list->count;
    if (available < num_modified_memrefs) {
      rc_modified_memref_list_t* new_modified_memref_list = (rc_modified_memref_list_t*)calloc(1, sizeof(rc_modified_memref_list_t));
      if (!new_modified_memref_list) {
        preparse->parse.offset = RC_OUT_OF_MEMORY;
        return;
      }

      new_size = rc_preparse_array_size(num_modified_memrefs - available, 16);
      if (new_size > 0xFFFF) {
        preparse->parse.offset = RC_INSUFFICIENT_BUFFER;
        return;
      }
      new_modified_memref_list->capacity = (uint16_t)new_size;
      new_modified_memref_list->items = (rc_modified_memref_t*)malloc(new_size * sizeof(rc_modified_memref_t));
      if (!new_modified_memref_list->items) {
        free(new_modified_memref_list);
        preparse->parse.offset = RC_OUT_OF_MEMORY;
        return;
      }

      new_modified_memref_list->allocated = 1;

      /* link it at the end, see above */
      while (modified_memref_list->next)
        modified_memref_list = modified_memref_list->next;
      modified_memref_list->next = new_modified_memref_list;
    }
  }

  preparse->parse.memrefs = memrefs;
}

static void rc_preparse_sync_operand(rc_operand_t* operand, rc_parse_state_t* parse, const rc_memrefs_t* memrefs)
{
  if (rc_operand_is_memref(operand) || rc_operand_is_recall(operand)) {
    const rc_memref_t* src_memref = operand->value.memref;
    if (!src_memref) {
      parse->offset = RC_INVALID_MEMORY_OPERAND;
      return;
    }

    if (src_memref->value.memref_type == RC_MEMREF_TYPE_MODIFIED_MEMREF) {
      const rc_modified_memref_list_t* modified_memref_list = &memrefs->modified_memrefs;
      for (; modified_memref_list; modified_memref_list = modified_memref_list->next) {
        const rc_modified_memref_t* modified_memref = modified_memref_list->items;
        const rc_modified_memref_t* modified_memref_end = modified_memref + modified_memref_list->count;

        for (; modified_memref < modified_memref_end; ++modified_memref) {
          if ((const rc_modified_memref_t*)src_memref == modified_memref) {
            rc_modified_memref_t* dst_modified_memref = rc_alloc_modified_memref(parse, modified_memref->memref.value.size,
              &modified_memref->parent, modified_memref->modifier_type, &modified_memref->modifier);

            operand->value.memref = &dst_modified_memref->memref;
            return;
          }
        }
      }
    }
    else {
      const rc_memref_list_t* memref_list = &memrefs->memrefs;
      for (; memref_list; memref_list = memref_list->next) {
        const rc_memref_t* memref = memref_list->items;
        const rc_memref_t* memref_end = memref + memref_list->count;

        for (; memref < memref_end; ++memref) {
          if (src_memref == memref) {
            operand->value.memref = rc_alloc_memref(parse, memref->address, memref->value.size);
            return;
          }
        }
      }
    }
  }
}

void rc_preparse_copy_memrefs(rc_parse_state_t* parse, const rc_memrefs_t* memrefs)
{
  const rc_memref_list_t* memref_list = &memrefs->memrefs;
  const rc_modified_memref_list_t* modified_memref_list = &memrefs->modified_memrefs;

  for (; memref_list; memref_list = memref_list->next) {
    const rc_memref_t* memref = memref_list->items;
    const rc_memref_t* memref_end = memref + memref_list->count;

    for (; memref < memref_end; ++memref) {
      rc_alloc_memref(parse, memref->address, memref->value.size);
      if (parse->offset < 0)
        return;
    }
  }

  for (; modified_memref_list; modified_memref_list = modified_memref_list->next) {
    rc_modified_memref_t* modified_memref = modified_memref_list->items;
    const rc_modified_memref_t* modified_memref_end = modified_memref + modified_memref_list->count;

    for (; modified_memref < modified_memref_end; ++modified_memref) {
      rc_preparse_sync_operand(&modified_memref->parent, parse, memrefs);
      rc_preparse_sync_operand(&modified_memref->modifier, parse, memrefs);
      if (parse->offset < 0)
        return;

      rc_alloc_modified_memref(parse, modified_memref->memref.value.size,
        &modified_memref->parent, modified_memref->modifier_type, &modified_memref->modifier);
    }
  }
}

void rc_reset_parse_state(rc_parse_state_t* parse, void* buffer, size_t buffer_size)
{
  parse->buffer = buffer;
  parse->buffer_size = (uint32_t)buffer_size;

  parse->offset = 0;

  parse->strings = NULL;

  parse->memrefs = NULL;
  parse->existing_memrefs = NULL;
  parse->variables = NULL;

  parse->addsource_parent.type = RC_OPERAND_NONE;
  parse->indirect_parent.type = RC_OPERAND_NONE;
  parse->remember.type = RC_OPERAND_NONE;

  parse->measured_target = 0;
  parse->lines_read = 0;

  parse->addsource_oper = RC_OPERATOR_NONE;
  parse->is_value = 0;
  parse->has_required_hits = 0;
  parse->measured_as_percent = 0;
  parse->ignore_non_parse_errors = 0;

  /* NOTE: cannot reset scratch_buffer as it contains the memrefs that need to be copied into the new buffer */

  parse->scratch = NULL;
}

void rc_init_parse_state(rc_parse_state_t* parse, void* buffer, size_t buffer_size)
{
  rc_buffer_init(&parse->scratch_buffer);

  rc_reset_parse_state(parse, buffer, buffer_size);
}

void rc_destroy_parse_state(rc_parse_state_t* parse)
{
  rc_buffer_destroy(&parse->scratch_buffer);
}

void rc_init_eval_state(rc_eval_state_t* eval_state, rc_read_memory_func_t read_memory, void* ud)
{
  memset(eval_state, 0, sizeof(*eval_state));
  eval_state->read_memory = read_memory;
  eval_state->read_memory_userdata = ud;
}
