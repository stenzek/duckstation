/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _LOG_H
#define _LOG_H

#include <cstdarg>

extern void Log(const char *fmt, ...);
extern void Logv(const char *fmt, std::va_list list);

#endif
