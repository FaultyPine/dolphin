/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _GGPO_WINDOWS_H_
#define _GGPO_WINDOWS_H_

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include "types.h"
#include "../BrawlbackUtility.h"


class Platform {
public:  // types
   typedef DWORD ProcessID;

public:  // functions
   static ProcessID GetProcessID() { return 0; }
   static void AssertFailed(char *msg) { ERROR_LOG(BRAWLBACK, "GGPO Assertion failed! %s\n", msg); }
   static u32 GetCurrentTimeMS() { return Common::Timer::GetTimeMs(); }
};

#endif
