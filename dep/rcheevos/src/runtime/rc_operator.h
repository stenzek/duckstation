#ifndef RC_RUNTIME_OPERATOR_H
#define RC_RUNTIME_OPERATOR_H

#include "rc_export.h"

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

enum {
  RC_OPERATOR_EQ,
  RC_OPERATOR_LT,
  RC_OPERATOR_LE,
  RC_OPERATOR_GT,
  RC_OPERATOR_GE,
  RC_OPERATOR_NE,
  RC_OPERATOR_NONE,
  RC_OPERATOR_MULT,
  RC_OPERATOR_DIV,
  RC_OPERATOR_AND,
  RC_OPERATOR_XOR,
  RC_OPERATOR_MOD,
  RC_OPERATOR_ADD,
  RC_OPERATOR_SUB,

  RC_OPERATOR_SUB_PARENT, /* internal use */
  RC_OPERATOR_ADD_ACCUMULATOR, /* internal use */
  RC_OPERATOR_SUB_ACCUMULATOR, /* internal use */
  RC_OPERATOR_INDIRECT_READ /* internal use */
};

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

int rc_operator_is_modifying(int oper);

RC_END_C_DECLS

#endif /* RC_RUNTIME_CONDITION_H */
