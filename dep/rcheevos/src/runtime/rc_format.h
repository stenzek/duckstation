#ifndef RC_RUNTIME_FORMAT_H
#define RC_RUNTIME_FORMAT_H

#include "rc_export.h"

#include <stddef.h>
#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

/* Supported formats. */
enum {
  RC_FORMAT_FRAMES,
  RC_FORMAT_SECONDS,
  RC_FORMAT_CENTISECS,
  RC_FORMAT_SCORE,
  RC_FORMAT_VALUE,
  RC_FORMAT_MINUTES,
  RC_FORMAT_SECONDS_AS_MINUTES,
  RC_FORMAT_FLOAT1,
  RC_FORMAT_FLOAT2,
  RC_FORMAT_FLOAT3,
  RC_FORMAT_FLOAT4,
  RC_FORMAT_FLOAT5,
  RC_FORMAT_FLOAT6,
  RC_FORMAT_FIXED1,
  RC_FORMAT_FIXED2,
  RC_FORMAT_FIXED3,
  RC_FORMAT_TENS,
  RC_FORMAT_HUNDREDS,
  RC_FORMAT_THOUSANDS,
  RC_FORMAT_UNSIGNED_VALUE,
  RC_FORMAT_UNFORMATTED
};

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_typed_value_t; /* rc_typed_value.h */

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Converts a format string to an RC_FORMAT_* enum value.
 */
int rc_parse_format(const char* format_str);

/**
 * Formats a `value` into a `buffer` using the specified `format`.
 *
 * Returns the number of characters written to the buffer. If the buffer is not large enough, returns the number of characters that would have been written.
 */
int rc_format_value(char buffer[], size_t buffer_size, int32_t value, int format);

/**
 * Formats a `value` into a `buffer` using the specified `format`.
 *
 * Returns the number of characters written to the buffer. If the buffer is not large enough, returns the number of characters that would have been written.
 */
int rc_format_typed_value(char buffer[], size_t buffer_size, const struct rc_typed_value_t* value, int format);

RC_END_C_DECLS

#endif /* RC_RUNTIME_FORMAT_H */
