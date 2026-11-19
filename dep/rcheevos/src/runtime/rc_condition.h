#ifndef RC_RUNTIME_CONDITION_H
#define RC_RUNTIME_CONDITION_H

#include "rc_operand.h"
#include "rc_operator.h" /* for condition.oper field */

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

enum {
  RC_CONDITION_STANDARD, /* this should always be 0 */
  RC_CONDITION_PAUSE_IF,
  RC_CONDITION_RESET_IF,
  RC_CONDITION_MEASURED_IF,
  RC_CONDITION_TRIGGER,
  RC_CONDITION_MEASURED,
  RC_CONDITION_ADD_SOURCE,
  RC_CONDITION_SUB_SOURCE,
  RC_CONDITION_ADD_ADDRESS,
  RC_CONDITION_REMEMBER,
  RC_CONDITION_ADD_HITS,
  RC_CONDITION_SUB_HITS,
  RC_CONDITION_RESET_NEXT_IF,
  RC_CONDITION_AND_NEXT,
  RC_CONDITION_OR_NEXT
};

enum {
  RC_PROCESSING_COMPARE_DEFAULT = 0,
  RC_PROCESSING_COMPARE_MEMREF_TO_CONST,
  RC_PROCESSING_COMPARE_MEMREF_TO_DELTA,
  RC_PROCESSING_COMPARE_MEMREF_TO_MEMREF,
  RC_PROCESSING_COMPARE_DELTA_TO_MEMREF,
  RC_PROCESSING_COMPARE_DELTA_TO_CONST,
  RC_PROCESSING_COMPARE_MEMREF_TO_CONST_TRANSFORMED,
  RC_PROCESSING_COMPARE_MEMREF_TO_DELTA_TRANSFORMED,
  RC_PROCESSING_COMPARE_MEMREF_TO_MEMREF_TRANSFORMED,
  RC_PROCESSING_COMPARE_DELTA_TO_MEMREF_TRANSFORMED,
  RC_PROCESSING_COMPARE_DELTA_TO_CONST_TRANSFORMED,
  RC_PROCESSING_COMPARE_ALWAYS_TRUE,
  RC_PROCESSING_COMPARE_ALWAYS_FALSE
};

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_eval_state_t;    /* rc_eval_state.h */
struct rc_parse_state_t;   /* rc_parse_state.h */
struct rc_typed_value_t;   /* rc_typed_value.h */

typedef struct rc_condition_t {
  /* The first operand. */
  struct rc_operand_t operand1;
  /* The second operand. */
  struct rc_operand_t operand2;

  /* Number of hits needed for this condition to be true. */
  uint32_t required_hits;
  /* Number of hits so far. */
  uint32_t current_hits;

  /* The next condition in the chain. */
  struct rc_condition_t* next;

  /* The type of the condition. (RC_CONDITION_*) */
  uint8_t type;

  /* The comparison operator to use. (RC_OPERATOR_*) */
  uint8_t oper; /* operator is a reserved word in C++. */

  /* Will be non-zero if the condition evaluated true on the last check.
   * - The lowest bit indicates whether the condition itself was true.
   * - The second lowest bit will only ever be set on ResetIf conditions.
   *   If set, it indicates that the condition was responsible for resetting the
   *   trigger. A reset clears all hit counts, so the condition may not appear to
   *   be true just from looking at it (in which case the lower bit will be 0).
   *   Also, the condition might have only met its required_hits target though
   *   an AddHits chain which will have also been reset.
   */
  uint8_t is_true;

  /* Unique identifier of optimized comparator to use. (RC_PROCESSING_COMPARE_*) */
  uint8_t optimized_comparator;
} rc_condition_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Returns non-zero if the `type` is associated to a memref.
 */
int rc_condition_is_combining(const struct rc_condition_t* self);

/**
 * Returns the actual first operand of a condition.
 *
 * For performance reasons, the operand in the condition may be the current node of a modified memref chain.
 */
const rc_operand_t* rc_condition_get_real_operand1(const struct rc_condition_t* self);

/**
 * [deprecated] Allocates and extracts an `rc_condition_t` from a serialized condition.
 */
rc_condition_t* rc_parse_condition(const char** memaddr, struct rc_parse_state_t* parse);

/**
 * Extracts an `rc_condition_t` from a serialized condition.
 */
void rc_parse_condition_internal(struct rc_condition_t* self, const char** memaddr, struct rc_parse_state_t* parse);

/**
 * Determines if the condition is logically true.
 */
int rc_test_condition(struct rc_condition_t* self, struct rc_eval_state_t* eval_state);

/**
 * Calculates the measured value of a condition.
 */
void rc_evaluate_condition_value(struct rc_typed_value_t* value, struct rc_condition_t* self, struct rc_eval_state_t* eval_state);

/**
 * Loads combining conditions into the modified_memref chain-builder parts of the parser.
 */
void rc_condition_update_parse_state(struct rc_condition_t* condition, struct rc_parse_state_t* parse);

RC_END_C_DECLS

#endif /* RC_RUNTIME_CONDITION_H */
