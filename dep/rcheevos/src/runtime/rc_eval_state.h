#ifndef RC_RUNTIME_EVAL_STATE_H
#define RC_RUNTIME_EVAL_STATE_H

#include "rc_export.h"
#include "rc_typed_value.h"

#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

/**
 * Callback used to read `num_bytes` bytes from memory starting at `address` into `buffer`.
 */
typedef uint32_t(RC_CCONV* rc_read_memory_func_t)(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud);

typedef struct rc_eval_state_t {
  /* ------------------------- */
  /* ===== memory access ===== */
  /* ------------------------- */

  /* callback function to actually do the read. */
  rc_read_memory_func_t read_memory;

  /* custom data to pass to the callback function. */
  void* read_memory_userdata;

  /* -------------------------------------- */
  /* ===== condition processing state ===== */
  /* -------------------------------------- */

  /* A captured Measured value. */
  rc_typed_value_t measured_value;

  /* AddHits/SubHits accumulator. */
  int32_t add_hits;

  /* True if all conditions are true. */
  uint8_t is_true;

  /* True if all non-Trigger conditions are true. */
  uint8_t is_primed;

  /* True if one or more PauseIf conditions is true. */
  uint8_t is_paused;

  /* False if the measured value should be ignored. */
  uint8_t can_measure;

  /* True if the measured_value came from a condition's hit count. */
  uint8_t measured_from_hits;

  /* True if the previous condition was AndNext true. */
  uint8_t and_next;

  /* True if the previous condition was OrNext true. */
  uint8_t or_next;

  /* True if the previous condition was ResetNextIf true. */
  uint8_t reset_next;

  /* True to abort the processing loop. */
  uint8_t stop_processing;

  /* ------------------------------------ */
  /* ===== trigger processing state ===== */
  /* ------------------------------------ */

  /* True if one of more hit counts is non-zero. */
  uint8_t has_hits;

  /* True if one or more ResetIf conditions is true. */
  uint8_t was_reset;

  /* True if one or more ResetNextIf conditions is true. */
  uint8_t was_cond_reset;

  /* ---------------------------- */
  /* ===== control settings ===== */
  /* ---------------------------- */

  /* Allows logic processing to stop as soon as a false condition is encountered. */
  uint8_t can_short_curcuit;
} rc_eval_state_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Reads up to 32-bits of data from the specified memory address.
 */
uint32_t rc_read_memory(uint32_t address, uint8_t size, rc_read_memory_func_t read_memory, void* ud);

/**
 * Initializes an evaluation state structure.
 */
void rc_init_eval_state(struct rc_eval_state_t* eval_state, rc_read_memory_func_t read_memory, void* ud);

RC_END_C_DECLS

#endif /* RC_RUNTIME_EVAL_STATE_H */
