#ifndef RC_RUNTIME_CONDSET_H
#define RC_RUNTIME_CONDSET_H

#include "rc_export.h"

#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_condition_t;   /* rc_condition.h */
struct rc_eval_state_t;  /* rc_eval_state.h */
struct rc_parse_state_t; /* rc_parse_state.h */

typedef struct rc_condset_t {
  /* The next condition set in the chain. */
  struct rc_condset_t* next;

  /* The first condition in this condition set. Then follow ->next chain. */
  struct rc_condition_t* conditions;

  /* The number of pause conditions in this condition set. */
  /* The first pause condition is at "this + RC_ALIGN(sizeof(this)). */
  uint16_t num_pause_conditions;

  /* The number of reset conditions in this condition set. */
  uint16_t num_reset_conditions;

  /* The number of hittarget conditions in this condition set. */
  uint16_t num_hittarget_conditions;

  /* The number of non-hittarget measured conditions in this condition set. */
  uint16_t num_measured_conditions;

  /* The number of other conditions in this condition set. */
  uint16_t num_other_conditions;

  /* The number of indirect conditions in this condition set. */
  uint16_t num_indirect_conditions;

  /* True if any condition in the set is a pause condition. */
  uint8_t has_pause; /* DEPRECATED - just check num_pause_conditions != 0 */
  /* True if the set is currently paused. */
  uint8_t is_paused;
} rc_condset_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Gets the conditions block of the condset.
 */
struct rc_condition_t* rc_condset_get_conditions(struct rc_condset_t* self);

/**
 * Allocates an `rc_condset_t` with the specified number of conditions.
 */
struct rc_condset_t* rc_alloc_condset(uint32_t num_conditions, struct rc_parse_state_t* parse);

/**
 * Allocates and extracts an `rc_condset_t` from a series of serialized conditions.
 */
struct rc_condset_t* rc_parse_condset(const char** memaddr, struct rc_parse_state_t* parse);

/**
 * Determines if the set of conditions is logically true.
 */
int rc_test_condset(struct rc_condset_t* self, struct rc_eval_state_t* eval_state);

/**
 * Evalute the truthiness of an array of conditions.
 */
void rc_test_condset_internal(struct rc_condition_t* condition, uint32_t num_conditions, struct rc_eval_state_t* eval_state, int can_short_circuit);

/**
 * Resets the captured hit count for every condition in the condset.
 */
void rc_reset_condset(struct rc_condset_t* self);

RC_END_C_DECLS

#endif /* RC_RUNTIME_TYPES_H */
