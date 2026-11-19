#ifndef RC_RUNTIME_VALIDATE_H
#define RC_RUNTIME_VALIDATE_H

#include "rc_export.h"

#include <stddef.h>
#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_condset_t; /* rc_condset.h */
struct rc_memrefs_t; /* rc_memrefs.h */
struct rc_trigger_t; /* rc_trigger.h */

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

int rc_validate_condset(const struct rc_condset_t* condset, char result[], const size_t result_size, uint32_t max_address);
int rc_validate_trigger(const struct rc_trigger_t* trigger, char result[], const size_t result_size, uint32_t max_address);

int rc_validate_condset_for_console(const struct rc_condset_t* condset, char result[], const size_t result_size, uint32_t console_id);
int rc_validate_trigger_for_console(const struct rc_trigger_t* trigger, char result[], const size_t result_size, uint32_t console_id);

int rc_validate_memrefs_for_console(const struct rc_memrefs_t* memrefs, char result[], const size_t result_size, uint32_t console_id);

RC_END_C_DECLS

#endif /* RC_RUNTIME_VALIDATE_H */
