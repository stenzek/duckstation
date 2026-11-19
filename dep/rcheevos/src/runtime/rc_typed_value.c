#include "rc_typed_value.h"

#include "rc_operator.h"

#include <float.h>  /* FLT_EPSILON */
#include <math.h>   /* fmod */
#include <string.h> /* memcpy */

void rc_typed_value_convert(rc_typed_value_t* value, uint8_t new_type) {
  switch (new_type) {
    case RC_VALUE_TYPE_UNSIGNED:
      switch (value->type) {
        case RC_VALUE_TYPE_UNSIGNED:
          return;
        case RC_VALUE_TYPE_SIGNED:
          value->value.u32 = (unsigned)value->value.i32;
          break;
        case RC_VALUE_TYPE_FLOAT:
          value->value.u32 = (unsigned)value->value.f32;
          break;
        default:
          value->value.u32 = 0;
          break;
      }
      break;

    case RC_VALUE_TYPE_SIGNED:
      switch (value->type) {
        case RC_VALUE_TYPE_SIGNED:
          return;
        case RC_VALUE_TYPE_UNSIGNED:
          value->value.i32 = (int)value->value.u32;
          break;
        case RC_VALUE_TYPE_FLOAT:
          value->value.i32 = (int)value->value.f32;
          break;
        default:
          value->value.i32 = 0;
          break;
      }
      break;

    case RC_VALUE_TYPE_FLOAT:
      switch (value->type) {
        case RC_VALUE_TYPE_FLOAT:
          return;
        case RC_VALUE_TYPE_UNSIGNED:
          value->value.f32 = (float)value->value.u32;
          break;
        case RC_VALUE_TYPE_SIGNED:
          value->value.f32 = (float)value->value.i32;
          break;
        default:
          value->value.f32 = 0.0;
          break;
      }
      break;

    default:
      break;
  }

  value->type = new_type;
}

static rc_typed_value_t* rc_typed_value_convert_into(rc_typed_value_t* dest, const rc_typed_value_t* source, char new_type) {
  memcpy(dest, source, sizeof(rc_typed_value_t));
  rc_typed_value_convert(dest, new_type);
  return dest;
}

void rc_typed_value_negate(rc_typed_value_t* value) {
  switch (value->type)
  {
    case RC_VALUE_TYPE_UNSIGNED:
      rc_typed_value_convert(value, RC_VALUE_TYPE_SIGNED);
      /* fallthrough */ /* to RC_VALUE_TYPE_SIGNED */

    case RC_VALUE_TYPE_SIGNED:
      value->value.i32 = -(value->value.i32);
      break;

    case RC_VALUE_TYPE_FLOAT:
      value->value.f32 = -(value->value.f32);
      break;

    default:
      break;
  }
}

void rc_typed_value_add(rc_typed_value_t* value, const rc_typed_value_t* amount) {
  rc_typed_value_t converted;

  if (amount->type != value->type && value->type != RC_VALUE_TYPE_NONE) {
    if (amount->type == RC_VALUE_TYPE_FLOAT)
      rc_typed_value_convert(value, RC_VALUE_TYPE_FLOAT);
    else
      amount = rc_typed_value_convert_into(&converted, amount, value->type);
  }

  switch (value->type)
  {
    case RC_VALUE_TYPE_UNSIGNED:
      value->value.u32 += amount->value.u32;
      break;

    case RC_VALUE_TYPE_SIGNED:
      value->value.i32 += amount->value.i32;
      break;

    case RC_VALUE_TYPE_FLOAT:
      value->value.f32 += amount->value.f32;
      break;

    case RC_VALUE_TYPE_NONE:
      memcpy(value, amount, sizeof(rc_typed_value_t));
      break;

    default:
      break;
  }
}

void rc_typed_value_multiply(rc_typed_value_t* value, const rc_typed_value_t* amount) {
  rc_typed_value_t converted;

  switch (value->type)
  {
    case RC_VALUE_TYPE_UNSIGNED:
      switch (amount->type)
      {
        case RC_VALUE_TYPE_UNSIGNED:
          /* the c standard for unsigned multiplication is well defined as non-overflowing truncation
           * to the type's size. this allows negative multiplication through twos-complements. i.e.
           *   1 * -1 (0xFFFFFFFF) = 0xFFFFFFFF = -1
           *   3 * -2 (0xFFFFFFFE) = 0x2FFFFFFFA & 0xFFFFFFFF = 0xFFFFFFFA = -6
           *  10 * -5 (0xFFFFFFFB) = 0x9FFFFFFCE & 0xFFFFFFFF = 0xFFFFFFCE = -50
           */
          value->value.u32 *= amount->value.u32;
          break;

        case RC_VALUE_TYPE_SIGNED:
          value->value.u32 *= (unsigned)amount->value.i32;
          break;

        case RC_VALUE_TYPE_FLOAT:
          rc_typed_value_convert(value, RC_VALUE_TYPE_FLOAT);
          value->value.f32 *= amount->value.f32;
          break;

        default:
          value->type = RC_VALUE_TYPE_NONE;
          break;
      }
      break;

    case RC_VALUE_TYPE_SIGNED:
      switch (amount->type)
      {
        case RC_VALUE_TYPE_SIGNED:
          value->value.i32 *= amount->value.i32;
          break;

        case RC_VALUE_TYPE_UNSIGNED:
          value->value.i32 *= (int)amount->value.u32;
          break;

        case RC_VALUE_TYPE_FLOAT:
          rc_typed_value_convert(value, RC_VALUE_TYPE_FLOAT);
          value->value.f32 *= amount->value.f32;
          break;

        default:
          value->type = RC_VALUE_TYPE_NONE;
          break;
      }
      break;

    case RC_VALUE_TYPE_FLOAT:
      if (amount->type == RC_VALUE_TYPE_NONE) {
        value->type = RC_VALUE_TYPE_NONE;
      }
      else {
        amount = rc_typed_value_convert_into(&converted, amount, RC_VALUE_TYPE_FLOAT);
        value->value.f32 *= amount->value.f32;
      }
      break;

    default:
      value->type = RC_VALUE_TYPE_NONE;
      break;
  }
}

void rc_typed_value_divide(rc_typed_value_t* value, const rc_typed_value_t* amount) {
  rc_typed_value_t converted;

  switch (amount->type)
  {
    case RC_VALUE_TYPE_UNSIGNED:
      if (amount->value.u32 == 0) { /* divide by zero */
        value->type = RC_VALUE_TYPE_NONE;
        return;
      }

      switch (value->type) {
        case RC_VALUE_TYPE_UNSIGNED: /* integer math */
          value->value.u32 /= amount->value.u32;
          return;
        case RC_VALUE_TYPE_SIGNED: /* integer math */
          value->value.i32 /= (int)amount->value.u32;
          return;
        case RC_VALUE_TYPE_FLOAT:
          amount = rc_typed_value_convert_into(&converted, amount, RC_VALUE_TYPE_FLOAT);
          break;
        default:
          value->type = RC_VALUE_TYPE_NONE;
          return;
      }
      break;

    case RC_VALUE_TYPE_SIGNED:
      if (amount->value.i32 == 0) { /* divide by zero */
        value->type = RC_VALUE_TYPE_NONE;
        return;
      }

      switch (value->type) {
        case RC_VALUE_TYPE_SIGNED: /* integer math */
          value->value.i32 /= amount->value.i32;
          return;
        case RC_VALUE_TYPE_UNSIGNED: /* integer math */
          value->value.u32 /= (unsigned)amount->value.i32;
          return;
        case RC_VALUE_TYPE_FLOAT:
          amount = rc_typed_value_convert_into(&converted, amount, RC_VALUE_TYPE_FLOAT);
          break;
        default:
          value->type = RC_VALUE_TYPE_NONE;
          return;
      }
      break;

    case RC_VALUE_TYPE_FLOAT:
      break;

    default:
      value->type = RC_VALUE_TYPE_NONE;
      return;
  }

  if (amount->value.f32 == 0.0) { /* divide by zero */
    value->type = RC_VALUE_TYPE_NONE;
    return;
  }

  rc_typed_value_convert(value, RC_VALUE_TYPE_FLOAT);
  value->value.f32 /= amount->value.f32;
}

void rc_typed_value_modulus(rc_typed_value_t* value, const rc_typed_value_t* amount) {
  rc_typed_value_t converted;

  switch (amount->type)
  {
    case RC_VALUE_TYPE_UNSIGNED:
      if (amount->value.u32 == 0) { /* divide by zero */
        value->type = RC_VALUE_TYPE_NONE;
        return;
      }

      switch (value->type) {
        case RC_VALUE_TYPE_UNSIGNED: /* integer math */
          value->value.u32 %= amount->value.u32;
          return;
        case RC_VALUE_TYPE_SIGNED: /* integer math */
          value->value.i32 %= (int)amount->value.u32;
          return;
        case RC_VALUE_TYPE_FLOAT:
          amount = rc_typed_value_convert_into(&converted, amount, RC_VALUE_TYPE_FLOAT);
          break;
        default:
          value->type = RC_VALUE_TYPE_NONE;
          return;
      }
      break;

    case RC_VALUE_TYPE_SIGNED:
      if (amount->value.i32 == 0) { /* divide by zero */
        value->type = RC_VALUE_TYPE_NONE;
        return;
      }

      switch (value->type) {
        case RC_VALUE_TYPE_SIGNED: /* integer math */
          value->value.i32 %= amount->value.i32;
          return;
        case RC_VALUE_TYPE_UNSIGNED: /* integer math */
          value->value.u32 %= (unsigned)amount->value.i32;
          return;
        case RC_VALUE_TYPE_FLOAT:
          amount = rc_typed_value_convert_into(&converted, amount, RC_VALUE_TYPE_FLOAT);
          break;
        default:
          value->type = RC_VALUE_TYPE_NONE;
          return;
      }
      break;

    case RC_VALUE_TYPE_FLOAT:
      break;

    default:
      value->type = RC_VALUE_TYPE_NONE;
      return;
  }

  if (amount->value.f32 == 0.0) { /* divide by zero */
    value->type = RC_VALUE_TYPE_NONE;
    return;
  }

  rc_typed_value_convert(value, RC_VALUE_TYPE_FLOAT);
  value->value.f32 = (float)fmod(value->value.f32, amount->value.f32);
}

void rc_typed_value_combine(rc_typed_value_t* value, rc_typed_value_t* amount, uint8_t oper) {
  switch (oper) {
    case RC_OPERATOR_MULT:
      rc_typed_value_multiply(value, amount);
      break;

    case RC_OPERATOR_DIV:
      rc_typed_value_divide(value, amount);
      break;

    case RC_OPERATOR_AND:
      rc_typed_value_convert(value, RC_VALUE_TYPE_UNSIGNED);
      rc_typed_value_convert(amount, RC_VALUE_TYPE_UNSIGNED);
      value->value.u32 &= amount->value.u32;
      break;

    case RC_OPERATOR_XOR:
      rc_typed_value_convert(value, RC_VALUE_TYPE_UNSIGNED);
      rc_typed_value_convert(amount, RC_VALUE_TYPE_UNSIGNED);
      value->value.u32 ^= amount->value.u32;
      break;

    case RC_OPERATOR_MOD:
      rc_typed_value_modulus(value, amount);
      break;

    case RC_OPERATOR_ADD:
      rc_typed_value_add(value, amount);
      break;

    case RC_OPERATOR_SUB:
      rc_typed_value_negate(amount);
      rc_typed_value_add(value, amount);
      break;
  }
}

static int rc_typed_value_compare_floats(float f1, float f2, char oper) {
  if (f1 == f2) {
    /* exactly equal */
  }
  else {
    /* attempt to match 7 significant digits (24-bit mantissa supports just over 7 significant decimal digits) */
    /* https://stackoverflow.com/questions/17333/what-is-the-most-effective-way-for-float-and-double-comparison */
    const float abs1 = (f1 < 0) ? -f1 : f1;
    const float abs2 = (f2 < 0) ? -f2 : f2;
    const float threshold = ((abs1 < abs2) ? abs1 : abs2) * FLT_EPSILON;
    const float diff = f1 - f2;
    const float abs_diff = (diff < 0) ? -diff : diff;

    if (abs_diff <= threshold) {
      /* approximately equal */
    }
    else if (diff > threshold) {
      /* greater */
      switch (oper) {
        case RC_OPERATOR_NE:
        case RC_OPERATOR_GT:
        case RC_OPERATOR_GE:
          return 1;

        default:
          return 0;
      }
    }
    else {
      /* lesser */
      switch (oper) {
        case RC_OPERATOR_NE:
        case RC_OPERATOR_LT:
        case RC_OPERATOR_LE:
          return 1;

        default:
          return 0;
      }
    }
  }

  /* exactly or approximately equal */
  switch (oper) {
    case RC_OPERATOR_EQ:
    case RC_OPERATOR_GE:
    case RC_OPERATOR_LE:
      return 1;

    default:
      return 0;
  }
}

int rc_typed_value_compare(const rc_typed_value_t* value1, const rc_typed_value_t* value2, uint8_t oper) {
  rc_typed_value_t converted_value;
  if (value2->type != value1->type) {
    /* if either side is a float, convert both sides to float. otherwise, assume the signed-ness of the left side. */
    if (value2->type == RC_VALUE_TYPE_FLOAT)
      value1 = rc_typed_value_convert_into(&converted_value, value1, value2->type);
    else
      value2 = rc_typed_value_convert_into(&converted_value, value2, value1->type);
  }

  switch (value1->type) {
    case RC_VALUE_TYPE_UNSIGNED:
      switch (oper) {
        case RC_OPERATOR_EQ: return value1->value.u32 == value2->value.u32;
        case RC_OPERATOR_NE: return value1->value.u32 != value2->value.u32;
        case RC_OPERATOR_LT: return value1->value.u32 < value2->value.u32;
        case RC_OPERATOR_LE: return value1->value.u32 <= value2->value.u32;
        case RC_OPERATOR_GT: return value1->value.u32 > value2->value.u32;
        case RC_OPERATOR_GE: return value1->value.u32 >= value2->value.u32;
        default: return 1;
      }

    case RC_VALUE_TYPE_SIGNED:
      switch (oper) {
        case RC_OPERATOR_EQ: return value1->value.i32 == value2->value.i32;
        case RC_OPERATOR_NE: return value1->value.i32 != value2->value.i32;
        case RC_OPERATOR_LT: return value1->value.i32 < value2->value.i32;
        case RC_OPERATOR_LE: return value1->value.i32 <= value2->value.i32;
        case RC_OPERATOR_GT: return value1->value.i32 > value2->value.i32;
        case RC_OPERATOR_GE: return value1->value.i32 >= value2->value.i32;
        default: return 1;
      }

    case RC_VALUE_TYPE_FLOAT:
      return rc_typed_value_compare_floats(value1->value.f32, value2->value.f32, oper);

    default:
      return 1;
  }
}
