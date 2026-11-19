#ifndef RC_RUNTIME_LBOARD_H
#define RC_RUNTIME_LBOARD_H

#include "rc_trigger.h"
#include "rc_value.h"

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

enum {
  RC_LBOARD_STATE_INACTIVE,  /* leaderboard is not being processed */
  RC_LBOARD_STATE_WAITING,   /* leaderboard cannot activate until the start condition has been false for at least one frame */
  RC_LBOARD_STATE_ACTIVE,    /* leaderboard is active and may start */
  RC_LBOARD_STATE_STARTED,   /* leaderboard attempt in progress */
  RC_LBOARD_STATE_CANCELED,  /* leaderboard attempt canceled */
  RC_LBOARD_STATE_TRIGGERED, /* leaderboard attempt complete, value should be submitted */
  RC_LBOARD_STATE_DISABLED   /* leaderboard cannot be processed at this time */
};

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_eval_state_t;  /* rc_eval_state.h */
struct rc_memrefs_t;     /* rc_modified_memrefs.h */
struct rc_parse_state_t; /* rc_parse_state.h */

typedef struct rc_lboard_t {
  /* The trigger that determines when to start the leaderboard. */
  rc_trigger_t start;

  /* The trigger that determines when to submit the leaderboard. */
  rc_trigger_t submit;

  /* The trigger that determines when to cancel the leaderboard. */
  rc_trigger_t cancel;

  /* The value reported by the leaderboard. */
  rc_value_t value;

  /* An alternate value to report while the leaderboard is active.
   *
   * This was only ever used by one or two games as the editor never supported it.
   */
  rc_value_t* progress;

  /* The current state of the leadeboard. */
  uint8_t state;

  /* True if the leaderboard has its own memrefs. */
  uint8_t has_memrefs;
} rc_lboard_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Returns non-zero if the provided leaderboard state allows the leaderboard to be processed.
 */
int rc_lboard_state_active(int state);

/**
 * Gets the memrefs owned by the leaderboard.
 *
 * Not valid if the leaderobard was parsed using `rc_parse_lboard_internal`.
 */
struct rc_memrefs_t* rc_lboard_get_memrefs(struct rc_lboard_t* self);

/**
 * Allocates space for a leaderboard that owns its own memrefs.
 */
struct rc_lboard_t* rc_alloc_lboard_with_memrefs(struct rc_parse_state_t* parse);

/**
 * Determines how much memory is needed to store the deserialized leaderboard.
 */
int rc_lboard_size(const char* memaddr);

/**
 * Deserializes a serialized leaderboard using a preallocated `buffer`.
 */
struct rc_lboard_t* rc_parse_lboard(void* buffer, const char* memaddr, void* unused_L, int unused_funcs_idx);

/**
 * Extracts an `rc_lboard_t` from a serialized leaderboard.
 */
void rc_parse_lboard_internal(struct rc_lboard_t* self, const char* memaddr, struct rc_parse_state_t* parse);

/**
 * Processes a leaderboard and returns the new state of the leaderboard (RC_LBOARD_STATE_*).
 */
int rc_evaluate_lboard(struct rc_lboard_t* lboard, int32_t* value, struct rc_eval_state_t* eval_state);

/**
 * Resets the captured hit count for every condition in the leaderboard.
 */
void rc_reset_lboard(struct rc_lboard_t* lboard);

RC_END_C_DECLS

#endif /* RC_RUNTIME_LBOARD_H */
