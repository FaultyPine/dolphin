// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/RollbackManager.h"

#include <algorithm>
#include <bitset>
#include <cstring>
#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/Rollback/Perf.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{

#if ROLLBACK_VALIDATE

// Physical offset of Brawl's "frames into current game" counter
static constexpr uint32_t BRAWL_FRAME_COUNTER_PHYS = 0x8062B420 & 0x1FFFFFFF;

static uint32_t ReadBrawlFrameCounter(const uint8_t* mem1_ptr, size_t mem1_size)
{
  if (!mem1_ptr || BRAWL_FRAME_COUNTER_PHYS + 4 > mem1_size)
    return 0;
  uint32_t raw;
  std::memcpy(&raw, mem1_ptr + BRAWL_FRAME_COUNTER_PHYS, sizeof(raw));
  return Common::swap32(raw);
}

void RollbackManager::CaptureValSnapshot(int slot)
{
  RollbackSnapshot& snap = m_val_snapshots[slot];

  if (!snap.mem1)
    snap.mem1 = std::make_unique<uint8_t[]>(m_mem1_size);
  std::memcpy(snap.mem1.get(), m_mem1_ptr, m_mem1_size);

  if (m_mem2_ptr && m_mem2_size > 0)
  {
    if (!snap.mem2)
      snap.mem2 = std::make_unique<uint8_t[]>(m_mem2_size);
    std::memcpy(snap.mem2.get(), m_mem2_ptr, m_mem2_size);
  }

  snap.brawl_frame = ReadBrawlFrameCounter(m_mem1_ptr, m_mem1_size);
  snap.valid = true;
}

void RollbackManager::CompareValSnapshot(int target_slot, int frames_back) const
{
  ROLLBACK_ZONE();
  const RollbackSnapshot& snap = m_val_snapshots[target_slot];
  if (!snap.valid || !snap.mem1)
  {
    OSD::AddMessage(fmt::format("VALIDATE: no snapshot for slot {}", target_slot), 3000,
                    OSD::Color::YELLOW);
    return;
  }

  uint32_t mem1_mismatch = 0;
  constexpr int MAX_LOG_PAGES = 8;
  uint32_t mismatch_pages[MAX_LOG_PAGES];
  const uint32_t mem1_page_count = static_cast<uint32_t>(m_mem1_size / PAGE_SIZE);

  for (uint32_t page = 0; page < mem1_page_count; ++page)
  {
    const size_t offset = static_cast<size_t>(page) * PAGE_SIZE;
    if (std::memcmp(m_mem1_ptr + offset, snap.mem1.get() + offset, PAGE_SIZE) != 0)
    {
      if (mem1_mismatch < MAX_LOG_PAGES)
        mismatch_pages[mem1_mismatch] = page;
      ++mem1_mismatch;
    }
  }

  uint32_t mem2_mismatch = 0;
  uint32_t mem2_mismatch_pages[MAX_LOG_PAGES];
  if (snap.mem2 && m_mem2_ptr && m_mem2_size > 0)
  {
    const uint32_t mem2_page_count = static_cast<uint32_t>(m_mem2_size / PAGE_SIZE);
    for (uint32_t page = 0; page < mem2_page_count; ++page)
    {
      const size_t offset = static_cast<size_t>(page) * PAGE_SIZE;
      if (std::memcmp(m_mem2_ptr + offset, snap.mem2.get() + offset, PAGE_SIZE) != 0)
      {
        if (mem2_mismatch < MAX_LOG_PAGES)
          mem2_mismatch_pages[mem2_mismatch] = page;
        ++mem2_mismatch;
      }
    }
  }

  const uint32_t current_brawl_frame = ReadBrawlFrameCounter(m_mem1_ptr, m_mem1_size);
  const bool frame_ok = (current_brawl_frame == snap.brawl_frame);
  const char* frame_tag = frame_ok ? "frame_ok" : "FRAME_MISMATCH";

  if (mem1_mismatch == 0 && mem2_mismatch == 0)
  {
    INFO_LOG_FMT(COMMON,
                 "[Rollback] VALIDATE OK  step={}  slot={}  brawl_frame={} (want {})  {}",
                 frames_back, target_slot, current_brawl_frame, snap.brawl_frame, frame_tag);
    return;
  }

  std::string mem1_list;
  const uint32_t mem1_logged = std::min(mem1_mismatch, static_cast<uint32_t>(MAX_LOG_PAGES));
  for (uint32_t i = 0; i < mem1_logged; ++i)
  {
    if (i) mem1_list += ", ";
    mem1_list += fmt::format("0x{:08x}", mismatch_pages[i] * static_cast<uint32_t>(PAGE_SIZE));
  }
  if (mem1_mismatch > MAX_LOG_PAGES)
    mem1_list += fmt::format(" (+{} more)", mem1_mismatch - MAX_LOG_PAGES);

  static constexpr uint32_t MEM2_PHYS_BASE = 0x10000000u;
  std::string mem2_list;
  const uint32_t mem2_logged = std::min(mem2_mismatch, static_cast<uint32_t>(MAX_LOG_PAGES));
  for (uint32_t i = 0; i < mem2_logged; ++i)
  {
    if (i) mem2_list += ", ";
    mem2_list += fmt::format("0x{:08x}",
        MEM2_PHYS_BASE + mem2_mismatch_pages[i] * static_cast<uint32_t>(PAGE_SIZE));
  }
  if (mem2_mismatch > MAX_LOG_PAGES)
    mem2_list += fmt::format(" (+{} more)", mem2_mismatch - MAX_LOG_PAGES);

  WARN_LOG_FMT(COMMON,
               "[Rollback] VALIDATE FAIL  step={}  slot={}  - {} MEM1 page(s) wrong, {} MEM2 "
               "page(s) wrong.  brawl_frame={} (want {})  {}\n  First MEM1 addrs: {}\n  First MEM2 addrs: {}",
               frames_back, target_slot, mem1_mismatch, mem2_mismatch,
               current_brawl_frame, snap.brawl_frame, frame_tag, mem1_list, mem2_list);
}

void RollbackManager::InvalidateValSnapshots()
{
  for (int i = 0; i < NUM_SAVE_SLOTS; ++i)
    m_val_snapshots[i].valid = false;
}

#endif  // ROLLBACK_VALIDATE

RollbackManager& RollbackManager::Get()
{
  static RollbackManager s_instance;
  return s_instance;
}

void RollbackManager::BeginDoState()
{
  m_skip_ram_in_dostate.store(true, std::memory_order_seq_cst);
  m_skip_jit_clear_in_dostate.store(true, std::memory_order_seq_cst);
  VideoCommon_SetSkipGPUReadbackForRollback(true);
  m_skip_ios_in_dostate.store(true, std::memory_order_seq_cst);
  PowerPC_SetSkipDCacheFlushForRollback(true);
}

void RollbackManager::EndDoState()
{
  PowerPC_SetSkipDCacheFlushForRollback(false);
  m_skip_ios_in_dostate.store(false, std::memory_order_seq_cst);
  VideoCommon_SetSkipGPUReadbackForRollback(false);
  m_skip_jit_clear_in_dostate.store(false, std::memory_order_seq_cst);
  m_skip_ram_in_dostate.store(false, std::memory_order_seq_cst);
}

void RollbackManager::Init(Core::System& system)
{
  if (m_initialized)
    Shutdown();

  PerfInit();

  auto& memory = system.GetMemory();
  m_mem1_ptr  = memory.GetRAM();
  m_mem1_size = memory.GetRamSize();
  m_mem2_ptr  = memory.GetEXRAM();
  m_mem2_size = memory.GetExRamSize();
  m_l1_cache_ptr  = memory.GetL1Cache();
  m_l1_cache_size = memory.GetL1CacheSize();

  for (int i = 0; i < NUM_SAVE_SLOTS; ++i)
    m_slots[i].Init(m_mem1_ptr, m_mem1_size, m_mem2_ptr, m_mem2_size,
                    m_l1_cache_ptr, m_l1_cache_size);

  JITDirtyBitmap::Get().Clear();

  m_ring_next  = 0;
  m_ring_count = 0;

  m_skip_ram_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_ios_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_jit_clear_in_dostate.store(false, std::memory_order_relaxed);
  m_frame_save_pending.store(false, std::memory_order_relaxed);
  m_initialized = true;

#if ROLLBACK_VALIDATE
  InvalidateValSnapshots();
#endif
}

void RollbackManager::Shutdown()
{
  if (!m_initialized)
    return;

  m_eviction_future = {};  // Wait for any in-flight eviction.

  m_base_snapshot.valid = false;
  m_base_snapshot.mem1.reset();
  m_base_snapshot.mem2.reset();

  JITDirtyBitmap::Get().Clear();

  for (int i = 0; i < NUM_SAVE_SLOTS; ++i)
    m_slots[i].Reset();

  m_ring_next  = 0;
  m_ring_count = 0;

  m_skip_ram_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_ios_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_jit_clear_in_dostate.store(false, std::memory_order_relaxed);
  m_frame_save_pending.store(false, std::memory_order_relaxed);
  m_frame_save_enabled.store(false, std::memory_order_relaxed);
  VideoCommon_SetSkipGPUReadbackForRollback(false);
  m_initialized = false;

#if ROLLBACK_VALIDATE
  InvalidateValSnapshots();
#endif
}

void RollbackManager::ToggleFrameSave()
{
  const bool enabled = !m_frame_save_enabled.load(std::memory_order_relaxed);
  m_frame_save_enabled.store(enabled, std::memory_order_relaxed);

  if (enabled)
  {
    m_ring_next  = 0;
    m_ring_count = 0;

    m_eviction_future = {};
    m_base_snapshot.valid = false;

    JITDirtyBitmap::Get().Clear();

#if ROLLBACK_VALIDATE
    InvalidateValSnapshots();
#endif
    OSD::AddMessage(
        fmt::format("Rollback: frame-save ON  ({} slots)", NUM_SAVE_SLOTS), 3000,
        OSD::Color::GREEN);
  }
  else
  {
    OSD::AddMessage("Rollback: frame-save OFF", 3000, OSD::Color::YELLOW);
  }
}

s32 Wrap(s32 x, s32 wrap)
{
  if (x < 0)
    x = (wrap + x);
  ASSERT(x >= 0);
  return x % wrap;
}

void RollbackManager::SaveFrame(Core::System& system)
{
  ROLLBACK_ZONE();
  if (!m_initialized)
    return;

  // Lazy init on the first frame, so we can be sure the game is fully booted when taking the base snapshot
  if (!m_base_snapshot.valid)
  {
    ROLLBACK_ZONE_N("BaseSnapshot::Init");
    std::unique_lock lk(m_base_snapshot.mutex);
    m_base_snapshot.mem1 = std::make_unique<uint8_t[]>(m_mem1_size);
    std::memcpy(m_base_snapshot.mem1.get(), m_mem1_ptr, m_mem1_size);
    if (m_mem2_ptr && m_mem2_size > 0)
    {
      m_base_snapshot.mem2 = std::make_unique<uint8_t[]>(m_mem2_size);
      std::memcpy(m_base_snapshot.mem2.get(), m_mem2_ptr, m_mem2_size);
    }
    m_base_snapshot.valid = true;
  }

  const int slot = m_ring_next;

  // Evict the oldest slot, async apply its deltas to the base snapshot
  if (m_ring_count == NUM_SAVE_SLOTS)
  {
    auto evicted = m_slots[slot].ExtractDeltas();
    m_eviction_future = std::async(std::launch::async,
        [this, evicted = std::move(evicted)]() mutable
        {
          ROLLBACK_THREAD_NAME("Rollback Eviction");
          ROLLBACK_ZONE_N("BaseSnapshot::Evict");
          std::unique_lock lk(m_base_snapshot.mutex);

          const uint8_t* src = evicted.mem1.page_data.data();
          for (uint32_t i = 0; i < evicted.mem1.page_count; ++i)
          {
            const size_t dst_off = static_cast<size_t>(evicted.mem1.page_indices[i]) * PAGE_SIZE;
            std::memcpy(m_base_snapshot.mem1.get() + dst_off, src + i * PAGE_SIZE, PAGE_SIZE);
          }

          if (m_base_snapshot.mem2)
          {
            src = evicted.mem2.page_data.data();
            for (uint32_t i = 0; i < evicted.mem2.page_count; ++i)
            {
              const size_t dst_off = static_cast<size_t>(evicted.mem2.page_indices[i]) * PAGE_SIZE;
              std::memcpy(m_base_snapshot.mem2.get() + dst_off, src + i * PAGE_SIZE, PAGE_SIZE);
            }
          }
        });
  }

  m_ring_next  = Wrap(m_ring_next + 1, NUM_SAVE_SLOTS);
  m_ring_count = std::clamp(m_ring_count + 1, 0, NUM_SAVE_SLOTS);

  m_slots[slot].Save(system);

#if ROLLBACK_VALIDATE
  CaptureValSnapshot(slot);
#endif
}

void RollbackManager::LoadFrame(Core::System& system, int frames_back)
{
  ROLLBACK_ZONE();
  if (!m_initialized)
    return;

  // >= 2 frames: the target (for non-RAM state) plus one older slot (for page deltas)
  if (m_ring_count < 2)
  {
    OSD::AddMessage(
        fmt::format("Cannot rollback (only {} frame(s) saved)", m_ring_count), 2000);
    return;
  }

  frames_back = std::clamp(frames_back, 1, m_ring_count - 1);

  const int most_recent = Wrap(m_ring_next - 1, NUM_SAVE_SLOTS);


  const int target_slot = Wrap(most_recent - frames_back, NUM_SAVE_SLOTS);

  // Gap pages: not in the target slot's delta (first written after the target frame).
  // The base snapshot holds their correct pre-target values.
  // We need to fetch these before applying deltas, which may mark pages dirty.
  std::bitset<JITDirtyBitmap::ENTRY_COUNT> target_pages;
  m_slots[target_slot].MarkTouchedGlobalPages(target_pages);

  // Apply deltas newest-to-oldest so pages in multiple slots end up at their target values.
  for (int step = 1; step <= frames_back; ++step)
  {
    const int slot = Wrap(most_recent - step, NUM_SAVE_SLOTS);
    m_slots[slot].ApplyDeltaReverse();
  }

  // Restore gap pages from base; read lock waits out any in-flight eviction
  {
    ROLLBACK_ZONE_N("GapPageRestore");
    std::shared_lock lk(m_base_snapshot.mutex);
    if (m_base_snapshot.valid)
    {
      const uint32_t mem1_pages = static_cast<uint32_t>(m_mem1_size / PAGE_SIZE);
      for (uint32_t p = 0; p < mem1_pages; ++p)
      {
        if (!target_pages.test(p))
          std::memcpy(m_mem1_ptr + p * PAGE_SIZE,
                      m_base_snapshot.mem1.get() + p * PAGE_SIZE, PAGE_SIZE);
      }
      if (m_mem2_ptr && m_base_snapshot.mem2)
      {
        const uint32_t mem2_pages = static_cast<uint32_t>(m_mem2_size / PAGE_SIZE);
        for (uint32_t p = 0; p < mem2_pages; ++p)
        {
          if (!target_pages.test(DeltaSaveSlot::MEM2_FIRST_PAGE + p))
            std::memcpy(m_mem2_ptr + p * PAGE_SIZE,
                        m_base_snapshot.mem2.get() + p * PAGE_SIZE, PAGE_SIZE);
        }
      }
    }
  }

#if ROLLBACK_VALIDATE
  CompareValSnapshot(target_slot, frames_back);
#endif

  bool ok = m_slots[target_slot].RestoreNonDeltaState(system);

  auto& bitmap = JITDirtyBitmap::Get();
  bitmap.ClearRange(0, static_cast<uint32_t>(m_mem1_size / PAGE_SIZE));
  if (m_mem2_ptr && m_mem2_size > 0)
    bitmap.ClearRange(DeltaSaveSlot::MEM2_FIRST_PAGE,
                      static_cast<uint32_t>(m_mem2_size / PAGE_SIZE));

  // After loading, the target slot becomes the new "oldest" slot,
  // so the next save will overwrite the next slot.
  m_ring_next  = Wrap(target_slot + 1, NUM_SAVE_SLOTS);
  m_ring_count = m_ring_count - frames_back;

  if (ok)
    OSD::AddMessage(fmt::format("Rolled back {} frame(s)", frames_back), 2000);
  else
    OSD::AddMessage("Rollback state load failed", 3000, OSD::Color::RED);
}

}  // namespace Rollback
