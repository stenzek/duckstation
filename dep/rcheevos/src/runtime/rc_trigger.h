#ifndef RC_RUNTIME_TRIGGER_H
#define RC_RUNTIME_TRIGGER_H

#include "rc_export.h"

#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

enum {
  RC_TRIGGER_STATE_INACTIVE,   /* achievement is not being processed */
  RC_TRIGGER_STATE_WAITING,    /* achievement cannot trigger until it has been false for at least one frame */
  RC_TRIGGER_STATE_ACTIVE,     /* achievement is active and may trigger */
  RC_TRIGGER_STATE_PAUSED,     /* achievement is currently paused and will not trigger */
  RC_TRIGGER_STATE_RESET,      /* achievement hit counts were reset */
  RC_TRIGGER_STATE_TRIGGERED,  /* achievement has triggered */
  RC_TRIGGER_STATE_PRIMED,     /* all non-Trigger conditions are true */
  RC_TRIGGER_STATE_DISABLED    /* achievement cannot be processed at this time */
};

#define RC_MEASURED_UNKNOWN 0xFFFFFFFF

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_condset_t;     /* rc_condition.h */
struct rc_eval_state_t;  /* rc_eval_state.h */
struct rc_memrefs_t;     /* rc_modified_memrefs.h */
struct rc_parse_state_t; /* rc_parse_state.h */

typedef struct rc_trigger_t {
  /* The main condition set. */
  struct rc_condset_t* requirement;

  /* The list of sub condition sets in this test. */
  struct rc_condset_t* alternative;

  /* The current state of the MEASURED condition. */
  uint32_t measured_value;

  /* The target state of the MEASURED condition */
  uint32_t measured_target;

  /* The current state of the trigger */
  uint8_t state;

  /* True if at least one condition has a non-zero hit count */
  uint8_t has_hits;

  /* True if the measured value should be displayed as a percentage */
  uint8_t measured_as_percent;

  /* True if the trigger has its own rc_memrefs_t */
  uint8_t has_memrefs;
} rc_trigger_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Returns non-zero if the provided trigger state allows the trigger to be processed.
 */
int rc_trigger_state_active(int state);

/**
 * Gets the memrefs owned by the trigger.
 *
 * Not valid if the trigger was parsed using `rc_parse_trigger_internal`.
 */
struct rc_memrefs_t* rc_trigger_get_memrefs(struct rc_trigger_t* self);

/**
 * Allocates space for a trigger that owns its own memrefs.
 */
struct rc_trigger_t* rc_alloc_trigger_with_memrefs(struct rc_parse_state_t* parse);

/**
 * Determines how much memory is needed to store the deserialized trigger.
 */
int rc_trigger_size(const char* memaddr);

/**
 * Deserializes a serialized trigger using a preallocated `buffer`.
 */
struct rc_trigger_t* rc_parse_trigger(void* buffer, const char* memaddr, void* unused_L, int unused_funcs_idx);

/**
 * Extracts an `rc_trigger_t` from a serialized trigger.
 */
void rc_parse_trigger_internal(struct rc_trigger_t* self, const char** memaddr, struct rc_parse_state_t* parse);

/**
 * Processes a trigger and returns the new state of the trigger (RC_TRIGGER_STATE_*).
 */
int rc_test_trigger(struct rc_trigger_t* self, struct rc_eval_state_t* eval_state);

/**
 * Resets the captured hit count for every condition in the trigger.
 */
void rc_reset_trigger(struct rc_trigger_t* self);

RC_END_C_DECLS

#endif /* RC_RUNTIME_TRIGGER_H */
