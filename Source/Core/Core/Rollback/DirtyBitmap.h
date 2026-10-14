// Copyright 2026 BrawlbackTeam
// SPDX-License-Identifier: GPL-2.0-or-later

// Dirty page bitmap for guest RAM, ported from FaultyPine's dolphin fork (DeltaSaveSlot.h).
//
// One byte per 4 KB page of the guest physical address space. Every write to guest RAM sets the
// byte for each page it touches, and rollback snapshots copy and clear the pages that are set. The
// JIT sets the bytes inline (EmuCodeBlock::EmitJITDirtyBitmapUpdate), so nothing traps and no
// page protection is needed. Writes that do not go through the JIT call MarkPhysicalRangeDirty.
//
// A marked page only costs a compare at the next snapshot, so marking is allowed to be
// conservative: a page that was read or marked without changing is harmless.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Common/CommonTypes.h"
#include "Common/Profiler.h"

namespace Rollback
{
// Guest physical pages are 4 KB regardless of the host page size.
constexpr std::size_t DIRTY_PAGE_SIZE = 4096;

// Plain global so the JIT can embed its address as an absolute immediate (see
// EmuCodeBlock::EmitJITDirtyBitmapUpdate).
struct alignas(64) JITDirtyBitmap
{
  static constexpr std::size_t ARENA_SIZE = 512ULL * 1024 * 1024;  // 512 MB
  static constexpr std::size_t ENTRY_COUNT = ARENA_SIZE / DIRTY_PAGE_SIZE;

  // Generated JIT code checks this before calculating a bitmap index. Keeping the check in the
  // generated block means already-compiled blocks safely switch as rollback arms and disarms.
  alignas(64) std::atomic<u8> tracking_enabled{0};
  // Read while compiling a JIT block. Full-scan benchmark mode disables emission of the dirty
  // update entirely, then invalidates existing blocks so replay execution has no tracking cost.
  std::atomic<u8> emit_updates{1};
  uint8_t entries[ENTRY_COUNT];

  static JITDirtyBitmap& Get()
  {
    static JITDirtyBitmap s_instance;
    return s_instance;
  }

  bool IsEnabled() const { return tracking_enabled.load(std::memory_order_acquire) != 0; }
  bool ShouldEmitUpdates() const { return emit_updates.load(std::memory_order_acquire) != 0; }
  void SetEmitUpdates(bool enabled)
  {
    emit_updates.store(enabled ? 1 : 0, std::memory_order_release);
  }
  void SetEnabled(bool enabled)
  {
    tracking_enabled.store(enabled ? 1 : 0, std::memory_order_release);
  }
  u8 Consume(std::size_t page)
  {
    return std::atomic_ref<u8>(entries[page]).exchange(0, std::memory_order_acq_rel);
  }
  u8 Load(std::size_t page) const
  {
    return std::atomic_ref<u8>(const_cast<u8&>(entries[page])).load(std::memory_order_acquire);
  }
  void Mark(std::size_t page)
  {
    std::atomic_ref<u8>(entries[page]).store(1, std::memory_order_release);
  }
  void Clear() { ClearRange(0, ENTRY_COUNT); }
  void ClearRange(uint32_t first_page, uint32_t page_count)
  {
    PROFILER_SCOPE_FUNCTION();
    for (std::size_t page = first_page; page < first_page + page_count; ++page)
      std::atomic_ref<u8>(entries[page]).store(0, std::memory_order_release);
  }
};

// Marks the guest physical range [physical_address, physical_address + size) as written. The
// address is masked to 29 bits, so virtual addresses (0x80000000, 0x90000000) work too.
inline void MarkPhysicalRangeDirty(u32 physical_address, std::size_t size)
{
  if (size == 0)
    return;
  auto& bitmap = JITDirtyBitmap::Get();
  if (!bitmap.IsEnabled())
    return;
  const std::size_t phys = physical_address & 0x1FFF'FFFFu;
  const std::size_t first = phys / DIRTY_PAGE_SIZE;
  std::size_t last = (phys + size - 1) / DIRTY_PAGE_SIZE;
  if (last >= JITDirtyBitmap::ENTRY_COUNT)
    last = JITDirtyBitmap::ENTRY_COUNT - 1;
  for (std::size_t page = first; page <= last; ++page)
    bitmap.Mark(page);
}
}  // namespace Rollback
