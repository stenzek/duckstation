#ifndef RC_RUNTIME_OPERAND_H
#define RC_RUNTIME_OPERAND_H

#include "rc_export.h"

#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

enum {
  RC_OPERAND_NONE = 0,       /* No operand */
  RC_OPERAND_ADDRESS,        /* The value of a live address in RAM. */
  RC_OPERAND_DELTA,          /* The value last known at this address. */
  RC_OPERAND_CONST,          /* A 32-bit unsigned integer. */
  RC_OPERAND_FP,             /* A floating point value. */
  RC_OPERAND_FUNC,           /* A function that provides the value. */
  RC_OPERAND_PRIOR,          /* The last differing value at this address. */
  RC_OPERAND_BCD,            /* The BCD-decoded value of a live address in RAM. */
  RC_OPERAND_INVERTED,       /* The twos-complement value of a live address in RAM. */
  RC_OPERAND_RECALL          /* The value captured by the last RC_CONDITION_REMEMBER condition */
};

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_eval_state_t;  /* rc_eval_state.h */
struct rc_memref_t;      /* rc_memref.h */
struct rc_parse_state_t; /* rc_parse_state.h */
struct rc_typed_value_t; /* rc_typed_value.h */

typedef struct rc_operand_t {
  union {
    /* A value read from memory. */
    struct rc_memref_t* memref;

    /* An integer value. */
    uint32_t num;

    /* A floating point value. */
    double dbl;
  } value;

  /* specifies which member of the value union is being used (RC_OPERAND_*) */
  uint8_t type;

  /* the RC_MEMSIZE of the operand specified in the condition definition - memref.size may differ */
  uint8_t size;

  /* specifies how to read the memref for some types (RC_OPERAND_*) */
  uint8_t memref_access_type;

  /* if set, this operand is combining the current condition with the previous one */
  uint8_t is_combining;
} rc_operand_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Returns non-zero if the `type` is associated to a memref.
 */
int rc_operand_type_is_memref(uint8_t type);

/**
 * Returns non-zero if a transformation needs to be applied to the value before it's returned by `rc_evaluate_operand`.
 */
int rc_operand_type_is_transform(uint8_t type);

/**
 * Returns non-zero if the `value.memref` field of the `operand` is valid.
 */
int rc_operand_is_memref(const struct rc_operand_t* operand);

/**
 * Returns non-zero if the `value.memref` field of the `operand` is valid and a float.
 */
int rc_operand_is_float_memref(const struct rc_operand_t* self);

/**
 * Returns non-zero if the operand will resolve to a float.
 */
int rc_operand_is_float(const struct rc_operand_t* self);

/**
 * Returns non-zero if the operand is a recalled value.
 */
int rc_operand_is_recall(const struct rc_operand_t* self);

/**
 * Extracts an `rc_operand_t` from a serialized operand.
 */
int rc_parse_operand(struct rc_operand_t* self, const char** memaddr, struct rc_parse_state_t* parse);

/**
 * Constructs an `rc_operand_t` for a constant integer.
 */
void rc_operand_set_const(struct rc_operand_t* self, uint32_t value);

/**
 * Constructs an `rc_operand_t` for a constant float.
 */
void rc_operand_set_float_const(struct rc_operand_t* self, double value);

/**
 * Gets the value of an operand.
 */
void rc_evaluate_operand(struct rc_typed_value_t* value, const struct rc_operand_t* self, struct rc_eval_state_t* eval_state);

/**
 * Determines if two operands are logically the same.
 */
int rc_operands_are_equal(const struct rc_operand_t* left, const struct rc_operand_t* right);

RC_END_C_DECLS

#endif /* RC_RUNTIME_OPERAND_H */
