// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Perf.h"

namespace Rollback
{
#if defined(HAVE_SUPERLUMINAL_PERFORMANCEAPI) && defined(ROLLBACK_PROFILE_SUPERLUMINAL)
PerformanceAPI_Functions g_perf_api = {};
#endif

void PerfInit()
{
#if defined(HAVE_SUPERLUMINAL_PERFORMANCEAPI) && defined(ROLLBACK_PROFILE_SUPERLUMINAL)
  if (!PerformanceAPI_LoadFrom(
          L"C:\\Program Files\\Superluminal\\Performance\\API\\dll\\x64\\PerformanceAPI.dll",
          &g_perf_api))
  {
    PerformanceAPI_LoadFrom(L"PerformanceAPI.dll", &g_perf_api);
  }
#endif
}

}  // namespace Rollback
