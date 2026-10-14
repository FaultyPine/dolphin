// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <list>
#include <mutex>
#include <string>

#include "CommonTypes.h"

#ifdef TRACY_ENABLE
// mGBA defines restrict as __restrict, which conflicts with Tracy's MSVC allocator attributes.
#ifdef _MSC_VER
#pragma push_macro("restrict")
#undef restrict
#endif
#include <tracy/TracyC.h>
#include <tracy/Tracy.hpp>
#ifdef _MSC_VER
#pragma pop_macro("restrict")
#endif
#endif

namespace Common
{
// A zone whose lifetime spans callbacks. Begin and End must run on the same thread,
// outside any scoped zones that would otherwise cross its boundaries.
class ProfilerZone
{
public:
  ProfilerZone() = default;
  ~ProfilerZone() { End(); }
  ProfilerZone(const ProfilerZone&) = delete;
  ProfilerZone& operator=(const ProfilerZone&) = delete;

#ifdef TRACY_ENABLE
  void Begin(TracyCZoneCtx context) { m_context = context; }
#endif
  void End()
  {
#ifdef TRACY_ENABLE
    if (m_context.active)
      TracyCZoneEnd(m_context);
    m_context = {};
#endif
  }
  void Value([[maybe_unused]] u64 value)
  {
#ifdef TRACY_ENABLE
    TracyCZoneValue(m_context, value);
#endif
  }

private:
#ifdef TRACY_ENABLE
  TracyCZoneCtx m_context{};
#endif
};

class Profiler
{
public:
  Profiler(std::string name);
  ~Profiler();

  static std::string ToString();

  void Start(u64* time, int* depth);
  void Stop(u64* time, int* depth);
  std::string Read();

  bool operator<(const Profiler& b) const;

private:
  static std::list<Profiler*> s_all_profilers;
  static std::mutex s_mutex;
  static u32 s_max_length;
  static u64 s_frame_time;
  static u64 s_usecs_frame;

  static std::string s_lazy_result;
  static int s_lazy_delay;

  std::mutex m_mutex;
  std::string m_name;
  u64 m_usecs;
  u64 m_usecs_min;
  u64 m_usecs_max;
  u64 m_usecs_quad;
  u64 m_calls;
};

class ProfilerExecuter
{
public:
  ProfilerExecuter(Profiler* profiler, u64* time, int* depth)
      : m_profiler(profiler), m_time(time), m_depth(depth)
  {
    m_profiler->Start(m_time, m_depth);
  }
  ~ProfilerExecuter() { m_profiler->Stop(m_time, m_depth); }

private:
  Profiler* m_profiler;
  u64* m_time;
  int* m_depth;
};
}  // namespace Common

#define PROFILE(name)                                                                              \
  static Common::Profiler prof_gen(name);                                                          \
  static thread_local u64 prof_time;                                                               \
  static thread_local int prof_depth;                                                              \
  Common::ProfilerExecuter prof_e(&prof_gen, &prof_time, &prof_depth);

#ifdef TRACY_ENABLE
#define PROFILER_SCOPE(name) ZoneScopedN(name)
#define PROFILER_VALUE(value) ZoneValue(value)
#define PROFILER_FRAME_MARK() FrameMark
#define PROFILER_FRAME_MARK_N(name) FrameMarkNamed(name)
#define PROFILER_THREAD_NAME(name) tracy::SetThreadName(name)
#define PROFILER_ZONE_BEGIN(zone, name)                                                             \
  do                                                                                              \
  {                                                                                               \
    TracyCZoneN(profiler_zone_context, name, 1);                                                    \
    (zone).Begin(profiler_zone_context);                                                           \
  } while (0)
#else
#define PROFILER_SCOPE(name) ((void)0)
#define PROFILER_VALUE(value) ((void)0)
#define PROFILER_FRAME_MARK() ((void)0)
#define PROFILER_FRAME_MARK_N(name) ((void)0)
#define PROFILER_THREAD_NAME(name) ((void)0)
#define PROFILER_ZONE_BEGIN(zone, name) ((void)0)
#endif

#define PROFILER_SCOPE_FUNCTION() PROFILER_SCOPE(__FUNCTION__)
