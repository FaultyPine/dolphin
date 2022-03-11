/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _GGPO_LINUX_H_
#define _GGPO_LINUX_H_

#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include "types.h"
#include "../BrawlbackUtility.h"


class Platform {
public:  // types
   typedef pid_t ProcessID;

public:  // functions
   static ProcessID GetProcessID() { return 1; }
   static void AssertFailed(char *msg) { ERROR_LOG(BRAWLBACK, "GGPO Assertion failed! %s\n", msg); }
   static u32 GetCurrentTimeMS() { return Common::Timer::GetTimeMs(); }
};

#endif
