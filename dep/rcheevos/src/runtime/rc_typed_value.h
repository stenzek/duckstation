#ifndef RC_RUNTIME_TYPED_VALUE_H
#define RC_RUNTIME_TYPED_VALUE_H

#include "rc_export.h"

#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

enum {
  RC_VALUE_TYPE_NONE,
  RC_VALUE_TYPE_UNSIGNED,
  RC_VALUE_TYPE_SIGNED,
  RC_VALUE_TYPE_FLOAT
};

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

typedef struct rc_typed_value_t {
  union {
    uint32_t u32;
    int32_t i32;
    float f32;
  } value;

  uint8_t type;
} rc_typed_value_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Converts an `rc_typed_value_t` to a different type.
 */
void rc_typed_value_convert(struct rc_typed_value_t* value, uint8_t new_type);

/**
 * Adds two `rc_typed_value_t`s.
 *
 * If the `amount` is a float, `value` will be converted to a float.
 * Otherwise, `amount` will be converted to `value`'s type.
 */
void rc_typed_value_add(struct rc_typed_value_t* value, const struct rc_typed_value_t* amount);

/**
 * Multiplies two `rc_typed_value_t`s.
 *
 * If the `amount` is a float, `value` will be converted to a float.
 * Otherwise, `amount` will be converted to `value`'s type.
 */
void rc_typed_value_multiply(struct rc_typed_value_t* value, const struct rc_typed_value_t* amount);

/**
 * Divides a `rc_typed_value_t` by another `rc_typed_value_t`.
 *
 * `amount` will be converted to `value`'s type.
 */
void rc_typed_value_divide(struct rc_typed_value_t* value, const struct rc_typed_value_t* amount);

/**
 * Divides a `rc_typed_value_t` by another `rc_typed_value_t` and captures the remainder.
 *
 * `amount` will be converted to `value`'s type.
 */
void rc_typed_value_modulus(struct rc_typed_value_t* value, const struct rc_typed_value_t* amount);

/**
 * Changes the sign of a value.
 *
 * If `value`'s type is unsigned, it will be changed to signed first.
 */
void rc_typed_value_negate(struct rc_typed_value_t* value);

/**
 * Combines two `rc_typed_value_t`s using the specified operator (RC_OPERATOR_*).
 */
void rc_typed_value_combine(struct rc_typed_value_t* value, struct rc_typed_value_t* amount, uint8_t oper);

/**
 * Compares two `rc_typed_value_t`s using the specified operator (RC_OPERATOR_*).
 *
 * If either value is a float, a float comparison will be performed.
 * Otherwise, the signed-ness of `value1` will be used.
 *
 * Returns non-zero if the comparison was true, or zero if it was false.
 */
int rc_typed_value_compare(const struct rc_typed_value_t* value1, const struct rc_typed_value_t* value2, uint8_t oper);

RC_END_C_DECLS

#endif /* RC_RUNTIME_TYPED_VALUE_H */
