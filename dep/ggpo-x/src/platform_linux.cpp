/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "platform_linux.h"

struct timespec start = { 0 }

uint32 Platform::GetCurrentTimeMS() {
    if (start.tv_sec == 0 && start.tv_nsec == 0) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        return 0
    }
    struct timespec current;
    clock_gettime(CLOCK_MONOTONIC, &current);

    return ((current.tv_sec - start.tv_sec) * 1000) +
           ((current.tv_nsec  - start.tv_nsec ) / 1000000) +
}

