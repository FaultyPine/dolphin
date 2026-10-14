// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/DirtyPages.h"

#include "Common/Profiler.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "Common/Buffer.h"
#include "Common/Logging/Log.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/Rollback/DirtyBitmap.h"
#include "Core/Rollback/UndoLog.h"
#include "Core/System.h"

namespace Rollback::DirtyPages
{
namespace
{
constexpr std::size_t PAGE = DIRTY_PAGE_SIZE;
constexpr std::size_t JIT_CACHE_LINE = 32;
constexpr u32 MEM2_PHYSICAL = 0x10000000u;
// Spare page buffers kept ready at each snapshot.
constexpr std::size_t RESERVE_PAGES = 2048;

// The JIT's inline bitmap stores exist only in the x86-64 backend. Other hosts keep full copies.
#if defined(_M_X86_64) || defined(__x86_64__)
constexpr bool JIT_STORES_TRACKED = true;
#else
constexpr bool JIT_STORES_TRACKED = false;
#endif

struct Tracker
{
  std::mutex lock;
  const void* owner = nullptr;
  Core::System* system = nullptr;
  std::vector<Area> areas;
  std::vector<std::size_t> area_first_page;  // global index of each area's first page
  std::size_t page_count = 0;
  // RAM as it was at the newest snapshot, per area. Pages the bitmap marks are compared with it.
  std::vector<Common::UniqueBuffer<u8>> mirrors;
  std::unique_ptr<UndoLog> log;
  // Generation marks replace a freshly allocated and zeroed page array on every restore.
  std::vector<u32> restore_marks;
  u32 restore_mark = 0;
  bool force_full_scan = false;
  Counters counters;
  // Ids stay unique across arms, so a stale id never names another ring's snapshot.
  u64 next_id = 1;
};

// Never destroyed: a static destructor may still use it at exit.
Tracker& T()
{
  static Tracker* const tracker = new Tracker;
  return *tracker;
}

// The area that holds global page `page`.
std::size_t AreaOf(const Tracker& t, std::size_t page)
{
  return static_cast<std::size_t>(
      std::upper_bound(t.area_first_page.begin(), t.area_first_page.end(), page) -
      t.area_first_page.begin() - 1);
}

u32 PhysicalOf(const Area& area, std::size_t page_in_area)
{
  return area.physical_address + static_cast<u32>(page_in_area * PAGE);
}

// Copies `contents` over the live page at `live` if they differ. Report the exact changed guest
// cache lines so the caller does not invalidate JIT code merely because unrelated data shares its
// 4 KiB dirty-tracking page.
void ReplacePage(u8* live, const u8* contents, u32 physical,
                 const std::function<void(u32, u32)>& changed)
{
  PROFILER_SCOPE_FUNCTION();
  if (std::memcmp(live, contents, PAGE) == 0)
    return;
  for (std::size_t offset = 0; offset < PAGE; offset += JIT_CACHE_LINE)
  {
    if (std::memcmp(live + offset, contents + offset, JIT_CACHE_LINE) != 0)
      changed(physical + static_cast<u32>(offset), static_cast<u32>(JIT_CACHE_LINE));
  }
  std::memcpy(live, contents, PAGE);
}

// Settles the pages written since the newest snapshot. A page whose bytes changed keeps its old
// bytes (the mirror's) in the newest log, and then the mirror takes the live bytes. With no
// snapshot there is no log to keep them in, so only the mirror moves.
void CommitDirtyPages(Tracker& t)
{
  PROFILER_SCOPE_FUNCTION();
  auto& bitmap = JITDirtyBitmap::Get();
  for (std::size_t a = 0; a < t.areas.size(); ++a)
  {
    const Area& area = t.areas[a];
    const std::size_t pages = area.size / PAGE;
    const std::size_t first_bitmap_page = area.physical_address / PAGE;
    for (std::size_t i = 0; i < pages; ++i)
    {
      const std::size_t bitmap_page = first_bitmap_page + i;
      // Clean pages dominate. Avoid an atomic read-modify-write unless the cheap load observes a
      // mark; a mark racing after a clean load remains set for the next snapshot.
      if (!t.force_full_scan &&
          (!bitmap.Load(bitmap_page) || !bitmap.Consume(bitmap_page)))
        continue;
      ++t.counters.dirty_pages;
      u8* const live = area.alias + i * PAGE;
      u8* const mirror = t.mirrors[a].data() + i * PAGE;
      if (std::memcmp(live, mirror, PAGE) == 0)
        continue;
      if (t.log->Record(t.area_first_page[a] + i, mirror))
        ++t.counters.pages_recorded;
      std::memcpy(mirror, live, PAGE);
    }
  }
}

void ResetLocked(Tracker& t)
{
  PROFILER_SCOPE_FUNCTION();
  auto& bitmap = JITDirtyBitmap::Get();
  bitmap.SetEnabled(false);
  bitmap.SetEmitUpdates(true);
  // Active-session blocks omit the per-store enabled test. Stop dispatching them before the
  // tracker is used again in its guarded state. ClearSafe is valid from inside a JIT block.
  if (t.system)
    t.system->GetJitInterface().ClearSafe();
  if (t.log)
    t.next_id = t.log->NextId();
  t.owner = nullptr;
  t.system = nullptr;
  t.areas.clear();
  t.area_first_page.clear();
  t.page_count = 0;
  t.mirrors.clear();
  t.restore_marks.clear();
  t.restore_mark = 0;
  t.force_full_scan = false;
  t.log.reset();
}
}  // namespace

bool Arm(const void* owner, const std::vector<Area>& areas, bool force_full_scan)
{
  PROFILER_SCOPE_FUNCTION();
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (t.owner)
    return t.owner == owner;
  if (!JIT_STORES_TRACKED || areas.empty())
    return false;

  t.areas = areas;
  t.area_first_page.clear();
  t.page_count = 0;
  for (const Area& area : areas)
  {
    if (!area.alias || area.size % PAGE != 0 || area.physical_address % PAGE != 0)
    {
      ResetLocked(t);
      return false;
    }
    t.area_first_page.push_back(t.page_count);
    t.page_count += area.size / PAGE;
  }
  t.mirrors.clear();
  for (const Area& area : areas)
  {
    t.mirrors.emplace_back();
    t.mirrors.back().reset(area.size);
    std::memcpy(t.mirrors.back().data(), area.alias, area.size);
  }
  t.log = std::make_unique<UndoLog>(PAGE, t.page_count, t.next_id);
  t.restore_marks.assign(t.page_count, 0);
  t.restore_mark = 0;
  t.force_full_scan = force_full_scan;
  t.log->Reserve(RESERVE_PAGES);
  t.owner = owner;
  // Everything written before this point is already in the mirror.
  auto& bitmap = JITDirtyBitmap::Get();
  bitmap.SetEmitUpdates(!force_full_scan);
  bitmap.Clear();
  bitmap.SetEnabled(!force_full_scan);
  NOTICE_LOG_FMT(CORE, "Rollback: dirty-page snapshots over {} KB of guest RAM (full_scan={})",
                 t.page_count * PAGE / 1024, force_full_scan);
  return true;
}

bool ArmForSystem(Core::System& system, const void* owner, bool force_full_scan)
{
  PROFILER_SCOPE_FUNCTION();
  if (!JIT_STORES_TRACKED)
    return false;
  auto& memory = system.GetMemory();
  // Page-table mappings reach RAM at addresses the bitmap does not index by physical page.
  if (memory.HasNonCanonicalMappingsForRollback())
  {
    NOTICE_LOG_FMT(CORE, "Rollback: dirty-page tracking off (noncanonical RAM mapping); using "
                         "full-copy snapshots");
    return false;
  }
  std::vector<Area> areas;
  u8* const alias = memory.GetRollbackAlias(false);
  if (!alias)
  {
    NOTICE_LOG_FMT(CORE, "Rollback: dirty-page tracking off (no RAM alias); using full copies");
    return false;
  }
  areas.push_back(Area{alias, 0, memory.GetRamSize()});
  if (memory.GetEXRAM())
  {
    u8* const exram_alias = memory.GetRollbackAlias(true);
    if (!exram_alias)
    {
      NOTICE_LOG_FMT(CORE, "Rollback: dirty-page tracking off (no MEM2 alias); using full copies");
      return false;
    }
    areas.push_back(Area{exram_alias, MEM2_PHYSICAL, memory.GetExRamSize()});
  }
  else if (!system.IsWii())
  {
    auto& dsp = system.GetDSP();
    areas.push_back(Area{dsp.GetARAMPtr(), GC_ARAM_PHYSICAL, dsp.GetARAMSize()});
    if (memory.GetFakeVMEM())
    {
      areas.push_back(Area{memory.GetFakeVMEM(), GC_FAKE_VMEM_PHYSICAL,
                           memory.GetFakeVMemSize()});
    }
  }
  if (!Arm(owner, areas, force_full_scan))
    return false;

  Tracker& t = T();
  std::lock_guard lock(t.lock);
  t.system = &system;
  // Existing blocks retain the guarded bitmap update and are safe for the remainder of the block
  // that armed tracking. Newly dispatched blocks are recompiled with the active-session fast path.
  system.GetJitInterface().ClearSafe();
  return true;
}

void Disarm(const void* owner)
{
  PROFILER_SCOPE_FUNCTION();
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.owner || t.owner != owner)
    return;
  ResetLocked(t);
}

bool IsArmedFor(const void* owner)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.owner && t.owner == owner;
}

bool IsTrackingArea(const u8* alias, u32 physical_address, u32 size)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.owner && std::any_of(t.areas.begin(), t.areas.end(), [&](const Area& area) {
           return area.alias == alias && area.physical_address == physical_address &&
                  area.size == size;
         });
}

u64 Snapshot()
{
  PROFILER_SCOPE_FUNCTION();
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log)
    return 0;
  t.log->Reserve(RESERVE_PAGES);
  // The pages written since the previous snapshot belong to that snapshot's log, so commit them
  // before the new log opens.
  CommitDirtyPages(t);
  return t.log->Open();
}

bool Has(u64 id)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.log && t.log->Has(id);
}

bool Restore(u64 id, const std::function<void(u32 physical_address, u32 length)>& changed,
             bool tracked_bitmap_clear)
{
  PROFILER_SCOPE_FUNCTION();
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log || !t.log->Has(id))
    return false;
  // With inline tracking disabled, first settle every write made since the newest snapshot.
  if (t.force_full_scan)
    CommitDirtyPages(t);
  // Pages with a pre-image in the logs from `id` on hold their bytes at `id` there.
  if (++t.restore_mark == 0)
  {
    std::fill(t.restore_marks.begin(), t.restore_marks.end(), 0);
    t.restore_mark = 1;
  }
  const u32 restore_mark = t.restore_mark;
  t.log->ForEachPreImage(id, [&](std::size_t page, const u8* pre_image) {
    t.restore_marks[page] = restore_mark;
    const std::size_t a = AreaOf(t, page);
    const Area& area = t.areas[a];
    const std::size_t i = page - t.area_first_page[a];
    ReplacePage(area.alias + i * PAGE, pre_image, PhysicalOf(area, i), changed);
    std::memcpy(t.mirrors[a].data() + i * PAGE, pre_image, PAGE);
  });
  // A page without a pre-image has not changed since the newest snapshot, so the mirror holds its
  // bytes at `id`. That includes pages written since then, which are still only in the bitmap.
  auto& bitmap = JITDirtyBitmap::Get();
  for (std::size_t a = 0; a < t.areas.size(); ++a)
  {
    const Area& area = t.areas[a];
    const std::size_t pages = area.size / PAGE;
    const std::size_t first_bitmap_page = area.physical_address / PAGE;
    for (std::size_t i = 0; i < pages; ++i)
    {
      if (!bitmap.Load(first_bitmap_page + i) ||
          t.restore_marks[t.area_first_page[a] + i] == restore_mark)
        continue;
      ReplacePage(area.alias + i * PAGE, t.mirrors[a].data() + i * PAGE, PhysicalOf(area, i),
                  changed);
    }
  }
  // Live RAM now matches the mirror again, so nothing is dirty. The benchmark path clears only
  // MEM1/MEM2 instead of all 512 MB represented by the address-indexed bitmap.
  if (t.force_full_scan)
  {
    // The bitmap is disabled and remained clear.
  }
  else if (tracked_bitmap_clear)
  {
    for (const Area& area : t.areas)
      bitmap.ClearRange(area.physical_address / PAGE, area.size / PAGE);
  }
  else
  {
    bitmap.Clear();
  }
  t.log->RewindTo(id);
  return true;
}

void Drop(u64 id)
{
  PROFILER_SCOPE_FUNCTION();
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (t.log)
    t.log->Drop(id);
}

void OnMappingsChanged(Core::System& system)
{
  PROFILER_SCOPE_FUNCTION();
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log)
    return;
  if (system.GetMemory().HasNonCanonicalMappingsForRollback())
  {
    // The JIT bitmap indexes a masked effective address. Arbitrary BAT or page-table mappings do
    // not preserve that relationship, so no tracked snapshot can still be trusted.
    ERROR_LOG_FMT(CORE, "Rollback: guest RAM gained a noncanonical mapping; every dirty-page "
                        "snapshot is dropped");
    ResetLocked(t);
  }
}

void StopTracking()
{
  PROFILER_SCOPE_FUNCTION();
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  ResetLocked(t);
}

Counters GetCounters()
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.counters;
}
}  // namespace Rollback::DirtyPages
