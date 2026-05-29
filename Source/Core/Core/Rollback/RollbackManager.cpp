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
#include <Core/State.h>

namespace Rollback
{

// Brawlback's GAME_FRAME->persistentFrameCounter: 0x901812a0 + 0x14 = 0x901812b4 (MEM2).
// MEM2 physical base is 0x10000000, so the byte offset within MEM2 is 0x001812b4.
static constexpr uint32_t BRAWL_FRAME_COUNTER_MEM2_OFFSET = 0x001812b4u;

static uint32_t ReadBrawlFrameCounter(const uint8_t* mem2_ptr, size_t mem2_size)
{
  if (!mem2_ptr || BRAWL_FRAME_COUNTER_MEM2_OFFSET + 4 > mem2_size)
    return 0;
  uint32_t raw;
  std::memcpy(&raw, mem2_ptr + BRAWL_FRAME_COUNTER_MEM2_OFFSET, sizeof(raw));
  return Common::swap32(raw);
}

void RollbackManager::CaptureFullRamSnapshot(RollbackSnapshot& snap)
{
  if (!snap.mem1)
    snap.mem1 = std::make_unique<uint8_t[]>(m_mem1_size);
  std::memcpy(snap.mem1.get(), m_mem1_ptr, m_mem1_size);

  if (m_mem2_ptr && m_mem2_size > 0)
  {
    if (!snap.mem2)
      snap.mem2 = std::make_unique<uint8_t[]>(m_mem2_size);
    std::memcpy(snap.mem2.get(), m_mem2_ptr, m_mem2_size);
  }

  snap.brawl_frame = ReadBrawlFrameCounter(m_mem2_ptr, m_mem2_size);
  snap.valid = true;
}

#if ROLLBACK_VALIDATE

void RollbackManager::CompareValSnapshot(int target_slot, int frames_back,
    const std::bitset<JITDirtyBitmap::ENTRY_COUNT>& target_pages) const
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
  uint32_t mem1_mismatch_delta = 0;
  uint32_t mem1_mismatch_gap   = 0;
  constexpr int MAX_LOG_PAGES = 8;
  uint32_t mismatch_pages[MAX_LOG_PAGES];
  bool     mem1_mismatch_is_delta[MAX_LOG_PAGES] = {};
  const uint32_t mem1_page_count = static_cast<uint32_t>(m_mem1_size / PAGE_SIZE);

  for (uint32_t page = 0; page < mem1_page_count; ++page)
  {
    const uint32_t page_phys = page * static_cast<uint32_t>(PAGE_SIZE);
    bool excluded = false;
    for (const auto& r : m_exclude_regions)
      if (page_phys >= r.phys_start && page_phys < r.phys_end) { excluded = true; break; }
    if (excluded)
      continue;

    const size_t offset = static_cast<size_t>(page) * PAGE_SIZE;
    if (std::memcmp(m_mem1_ptr + offset, snap.mem1.get() + offset, PAGE_SIZE) != 0)
    {
      const bool in_delta = target_pages.test(page);
      if (in_delta) ++mem1_mismatch_delta; else ++mem1_mismatch_gap;
      if (mem1_mismatch < MAX_LOG_PAGES)
      {
        mismatch_pages[mem1_mismatch] = page;
        mem1_mismatch_is_delta[mem1_mismatch] = in_delta;
      }
      ++mem1_mismatch;
    }
  }

  uint32_t mem2_mismatch = 0;
  uint32_t mem2_mismatch_delta = 0;   // wrong pages that were in target delta
  uint32_t mem2_mismatch_gap   = 0;   // wrong pages that came from base snapshot
  uint32_t mem2_mismatch_pages[MAX_LOG_PAGES] = {};
  bool     mem2_mismatch_is_delta[MAX_LOG_PAGES] = {};
  if (snap.mem2 && m_mem2_ptr && m_mem2_size > 0)
  {
    const uint32_t mem2_page_count = static_cast<uint32_t>(m_mem2_size / PAGE_SIZE);
    for (uint32_t page = 0; page < mem2_page_count; ++page)
    {
      const uint32_t page_phys = 0x10000000u + page * static_cast<uint32_t>(PAGE_SIZE);
      bool excluded = false;
      for (const auto& r : m_exclude_regions)
        if (page_phys >= r.phys_start && page_phys < r.phys_end) { excluded = true; break; }
      if (excluded)
        continue;

      const size_t offset = static_cast<size_t>(page) * PAGE_SIZE;
      if (std::memcmp(m_mem2_ptr + offset, snap.mem2.get() + offset, PAGE_SIZE) != 0)
      {
        const bool in_delta = target_pages.test(DeltaSaveSlot::MEM2_FIRST_PAGE + page);
        if (in_delta) ++mem2_mismatch_delta; else ++mem2_mismatch_gap;
        if (mem2_mismatch < MAX_LOG_PAGES)
        {
          mem2_mismatch_pages[mem2_mismatch] = page;
          mem2_mismatch_is_delta[mem2_mismatch] = in_delta;
        }
        ++mem2_mismatch;
      }
    }
  }

  const uint32_t current_brawl_frame = ReadBrawlFrameCounter(m_mem2_ptr, m_mem2_size);
  const bool frame_ok = (current_brawl_frame == snap.brawl_frame);
  const char* frame_tag = frame_ok ? "frame_ok" : "FRAME_MISMATCH";

  if (mem1_mismatch == 0 && mem2_mismatch == 0)
  {
    const ExcludeRegion& stack_excl = m_exclude_regions.back();
    INFO_LOG_FMT(COMMON,
                 "[Rollback] VALIDATE OK  step={}  slot={}  brawl_frame={} (want {})  {}  "
                 "stack_excl=[0x{:08x},0x{:08x})",
                 frames_back, target_slot, current_brawl_frame, snap.brawl_frame, frame_tag,
                 stack_excl.phys_start, stack_excl.phys_end);
    return;
  }

  std::string mem1_list;
  const uint32_t mem1_logged = std::min(mem1_mismatch, static_cast<uint32_t>(MAX_LOG_PAGES));
  for (uint32_t i = 0; i < mem1_logged; ++i)
  {
    if (i) mem1_list += ", ";
    mem1_list += fmt::format("0x{:08x}({})",
        mismatch_pages[i] * static_cast<uint32_t>(PAGE_SIZE),
        mem1_mismatch_is_delta[i] ? "delta" : "gap");
  }
  if (mem1_mismatch > MAX_LOG_PAGES)
    mem1_list += fmt::format(" (+{} more)", mem1_mismatch - MAX_LOG_PAGES);

  std::string mem2_list;
  {
  const uint32_t mem2_logged = std::min(mem2_mismatch, static_cast<uint32_t>(MAX_LOG_PAGES));
    static constexpr uint32_t MEM2_PHYS_BASE = 0x10000000u;
  for (uint32_t i = 0; i < mem2_logged; ++i)
  {
    if (i) mem2_list += ", ";
      mem2_list += fmt::format("0x{:08x}({})",
          MEM2_PHYS_BASE + mem2_mismatch_pages[i] * static_cast<uint32_t>(PAGE_SIZE),
          mem2_mismatch_is_delta[i] ? "delta" : "gap");
  }
  if (mem2_mismatch > MAX_LOG_PAGES)
    mem2_list += fmt::format(" (+{} more)", mem2_mismatch - MAX_LOG_PAGES);
  }

  // Also show the live stack exclusion zone so we can tell whether wrong pages
  // are just outside it (which would explain state corruption on return).
  const ExcludeRegion& stack_excl = m_exclude_regions.back();
  WARN_LOG_FMT(COMMON,
               "[Rollback] VALIDATE FAIL  step={}  slot={}  - {} MEM1({} delta, {} gap) + "
               "{} MEM2({} delta, {} gap) page(s) wrong.  "
               "brawl_frame={} (want {})  {}  stack_excl=[0x{:08x},0x{:08x})\n"
               "  First MEM1 addrs: {}\n  First MEM2 addrs: {}",
               frames_back, target_slot,
               mem1_mismatch, mem1_mismatch_delta, mem1_mismatch_gap,
               mem2_mismatch, mem2_mismatch_delta, mem2_mismatch_gap,
               current_brawl_frame, snap.brawl_frame, frame_tag,
               stack_excl.phys_start, stack_excl.phys_end,
               mem1_list, mem2_list);
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
  PowerPC_SetSkipCPURegsForRollback(true);
}

void RollbackManager::EndDoState()
{
  PowerPC_SetSkipCPURegsForRollback(false);
  PowerPC_SetSkipDCacheFlushForRollback(false);
  m_skip_ios_in_dostate.store(false, std::memory_order_seq_cst);
  VideoCommon_SetSkipGPUReadbackForRollback(false);
  m_skip_jit_clear_in_dostate.store(false, std::memory_order_seq_cst);
  m_skip_ram_in_dostate.store(false, std::memory_order_seq_cst);
}

void RollbackManager::AddExcludeRegion(uint32_t virt_addr, uint32_t size_bytes)
{
  INFO_LOG_FMT(BRAWLBACK, "Added exclude region {} - {}", virt_addr, virt_addr + size_bytes);
  m_exclude_regions.push_back(ExcludeRegion::FromVirt(virt_addr, size_bytes));
}

static const std::vector<ExcludeRegion> s_brawlback_hardcoded_exclude_regions = {
  // Brawlback C++ framework heap (MEM2).
  // This holds rollback control state (framesToAdvance, pastFrameDatas, etc.)
  // that must survive across a rollback restore unchanged.
  //ExcludeRegion::FromVirt(0x935d7660u, 0x89a0u),
  ExcludeRegion::FromVirt(0x935d3940u, 0x0000c6c0),
  // default gecko codes region (do we actually want to exclude this? probably not...
  // ExcludeRegion::FromVirt(0x80001800, 0x80003000),

  // bss/data sections of our cpp code framework
  // see "infoSegmentAddress" - "memoryHeapEndAddress in settings.json in the BuildSystem
  ExcludeRegion::FromVirt(0x935D0000, 0x10000),
};

void RollbackManager::Init(Core::System& system)
{
  if (m_initialized)
    Shutdown();

  // Initialize WSQ job system once; workers persist across save/load cycles.
  if (!m_dispatch_thread)
  {
    m_job_ctx.activate();
    // Worker 0 is "owned" by the rollback thread — used for job creation/dispatch.
    // All initialize_worker calls must be sequential (no thread safety in ctx setup).
    m_dispatch_thread = m_job_ctx.initialize_worker(0, nullptr);
    for (int i = 1; i <= ROLLBACK_NUM_HELPER_THREADS; ++i)
    {
      job::JobTaskThread* thr = m_job_ctx.initialize_worker(
          static_cast<int64_t>(i) * 0x9e3779b97f4a7c15LL, nullptr);
      m_worker_threads.emplace_back([thr]() {
        ROLLBACK_THREAD_NAME("Rollback Job Pool");
        thr->wait_for_termination();
      });
    }
  }

  m_exclude_regions = s_brawlback_hardcoded_exclude_regions;
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

  if (m_eviction_future.valid())
    m_eviction_future.wait();

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

  // Signal WSQ workers to exit and wait for them.
  // Workers are persistent — only tear them down on full shutdown.
  if (m_dispatch_thread)
  {
    m_job_ctx.deactivate();
    for (auto& t : m_worker_threads)
      if (t.joinable()) t.join();
    m_worker_threads.clear();
    m_dispatch_thread = nullptr;
  }

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

    if (m_eviction_future.valid()) m_eviction_future.wait();
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
    CaptureFullRamSnapshot(m_base_snapshot);
    INFO_LOG_FMT(BRAWLBACK, "Captured base snapshot at brawl frame {}", m_base_snapshot.brawl_frame);
  }

  const int slot = m_ring_next;

  // Evict the oldest slot, async apply its deltas to the base snapshot
  if (m_ring_count == NUM_SAVE_SLOTS)
  {
    // Wait for any in-flight eviction — typically completes within the same frame.
    if (m_eviction_future.valid()) m_eviction_future.wait();
    auto evicted = std::make_shared<Rollback::EvictedDelta>(m_slots[slot].ExtractDeltas());
    m_eviction_future = std::async(std::launch::async,
      [this, evicted]() mutable
      {
        ROLLBACK_THREAD_NAME("Rollback Eviction");
        ROLLBACK_ZONE_N("BaseSnapshot::Evict");
        std::unique_lock lk(m_base_snapshot.mutex);

        const uint8_t* src = evicted->mem1.page_data.data();
        for (uint32_t i = 0; i < evicted->mem1.page_count; ++i)
        {
          const size_t dst_off = static_cast<size_t>(evicted->mem1.page_indices[i]) * PAGE_SIZE;
          std::memcpy(m_base_snapshot.mem1.get() + dst_off, src + i * PAGE_SIZE, PAGE_SIZE);
        }

        if (m_base_snapshot.mem2)
        {
          src = evicted->mem2.page_data.data();
          for (uint32_t i = 0; i < evicted->mem2.page_count; ++i)
          {
            const size_t dst_off = static_cast<size_t>(evicted->mem2.page_indices[i]) * PAGE_SIZE;
            std::memcpy(m_base_snapshot.mem2.get() + dst_off, src + i * PAGE_SIZE, PAGE_SIZE);
          }
        }
      });
  }

  m_ring_next  = Wrap(m_ring_next + 1, NUM_SAVE_SLOTS);
  m_ring_count = std::clamp(m_ring_count + 1, 0, NUM_SAVE_SLOTS);

  m_slots[slot].Save(system);

#if ROLLBACK_VALIDATE
  RollbackSnapshot& snap = m_val_snapshots[slot];
  CaptureFullRamSnapshot(snap);
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

  {
    ROLLBACK_ZONE_N("Preserve stack");
    // Preserve the live call stack in RAM so execution continues normally
    // after this load returns.  On PPC, r1 is the stack pointer and active
    // frames live at addresses >= r1 (callers are above the current frame).
    // We exclude those physical pages from the RAM restore so the function
    // call chain that issued CMD_LOAD_SAVESTATE stays intact.
    //
    // Use the OS thread struct to find the exact stack top rather than
    // assuming a fixed exclusion size (which could under- or over-cover).
    // DAT_800000e4 (physical 0xe4) = current OSThread pointer (virtual).
    // OSThread+0x304 = initialStackAddr (the HIGH end of the stack buffer).
    const uint32_t r1_virt = system.GetPowerPC().GetPPCState().gpr[1];
    const uint32_t r1_phys = r1_virt & 0x1FFF'FFFFu;
    const uint32_t stack_page = r1_phys & ~(static_cast<uint32_t>(PAGE_SIZE) - 1u);

    uint32_t stack_exclude_end = 0;
    ASSERT(m_mem1_size > 0xe4 + 4);
    uint32_t thread_virt;
    std::memcpy(&thread_virt, m_mem1_ptr + 0xe4, 4);
    thread_virt = Common::swap32(thread_virt);
    const uint32_t thread_phys = thread_virt & 0x1FFF'FFFFu;
    if (thread_phys + 0x308 <= m_mem1_size)
    {
      uint32_t stack_top_virt;
      std::memcpy(&stack_top_virt, m_mem1_ptr + thread_phys + 0x304, 4);
      stack_top_virt = Common::swap32(stack_top_virt);
      const uint32_t stack_top_phys = stack_top_virt & 0x1FFF'FFFFu;
      // Round up to next page boundary so the top page is fully covered.
      const uint32_t stack_top_page = (stack_top_phys + static_cast<uint32_t>(PAGE_SIZE) - 1u) &
                                      ~(static_cast<uint32_t>(PAGE_SIZE) - 1u);
      if (stack_top_page > stack_page && stack_top_page <= static_cast<uint32_t>(m_mem1_size))
        stack_exclude_end = stack_top_page;
    }
    ASSERT(stack_exclude_end != 0);

    INFO_LOG_FMT(BRAWLBACK,
                 "[Rollback] stack exclude: r1=0x{:08x} phys=[0x{:08x}, 0x{:08x}) ({} KB)", r1_virt,
                 stack_page, stack_exclude_end, (stack_exclude_end - stack_page) / 1024);
    m_exclude_regions.push_back(ExcludeRegion{stack_page, stack_exclude_end});
  }

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
    Rollback::DeltaSaveSlot& delta = m_slots[slot];
    {
      ROLLBACK_ZONE_N("mem1 delta apply");
      auto x = StringFromFormat("page count %u", delta.m_mem1_delta.page_count);
      ZoneText(x.c_str(), x.size());
      RestoreRegionDelta(delta.m_mem1_delta, m_mem1_ptr, 0u, m_exclude_regions);
    }
    {
      ROLLBACK_ZONE_N("mem2 delta apply");
      auto x = StringFromFormat("page count %u", delta.m_mem2_delta.page_count);
      ZoneText(x.c_str(), x.size());
      if (m_mem2_ptr && delta.m_mem2_page_count > 0)
        RestoreRegionDelta(delta.m_mem2_delta, m_mem2_ptr, MEM2_BASE, m_exclude_regions);
    }
  }

  // Restore gap pages from base; read lock waits out any in-flight eviction
  {
    ROLLBACK_ZONE_N("GapPageRestore");
    int numPagesToRestore = 0;
    std::shared_lock lk(m_base_snapshot.mutex);
    if (m_base_snapshot.valid)
    {
      const uint32_t mem1_pages = static_cast<uint32_t>(m_mem1_size / PAGE_SIZE);
      for (uint32_t p = 0; p < mem1_pages; ++p)
      {
        if (!target_pages.test(p))
        {
          savestateMemcpy(m_mem1_ptr + p * PAGE_SIZE,
                          m_base_snapshot.mem1.get() + p * PAGE_SIZE,
                          PAGE_SIZE,
                          p * static_cast<uint32_t>(PAGE_SIZE),
                          m_exclude_regions);
          numPagesToRestore++;
        }
      }
      if (m_mem2_ptr && m_base_snapshot.mem2)
      {
        const uint32_t mem2_pages = static_cast<uint32_t>(m_mem2_size / PAGE_SIZE);
        for (uint32_t p = 0; p < mem2_pages; ++p)
        {
          if (!target_pages.test(MEM2_FIRST_PAGE + p))
          {
            savestateMemcpy(m_mem2_ptr + p * PAGE_SIZE,
                            m_base_snapshot.mem2.get() + p * PAGE_SIZE,
                            PAGE_SIZE,
                            0x10000000u + p * static_cast<uint32_t>(PAGE_SIZE),
                            m_exclude_regions);
            numPagesToRestore++;
          }
        }
      }
    }
    auto x = StringFromFormat("Restored %u gap page(s) from base snapshot", numPagesToRestore);
    ZoneText(x.c_str(), x.size());
  }

  DeltaSaveSlot& deltaSave = m_slots[target_slot];
  {
    ROLLBACK_ZONE_N("L1 cache restore");
    if (m_l1_cache_ptr && m_l1_cache_size > 0 && deltaSave.m_l1_cache_snapshot.data())
      std::memcpy(m_l1_cache_ptr, deltaSave.m_l1_cache_snapshot.data(), m_l1_cache_size);
  }

  bool ok = false;
  {
    ROLLBACK_ZONE_N("DoState restore");
    BeginDoState();
    ok = State::LoadFromBuffer(
        system, std::span<uint8_t>(deltaSave.m_save_buffer.data(), deltaSave.m_save_buffer.size()));
    EndDoState();
  }

#if ROLLBACK_VALIDATE
  CompareValSnapshot(target_slot, frames_back, target_pages);
#endif

  // Remove the temporary live-stack exclusion (always the last element pushed).
  m_exclude_regions.pop_back();

  auto& bitmap = JITDirtyBitmap::Get();
  bitmap.ClearRange(0, static_cast<uint32_t>(m_mem1_size / PAGE_SIZE));
  if (m_mem2_ptr && m_mem2_size > 0)
    bitmap.ClearRange(MEM2_FIRST_PAGE,
                      static_cast<uint32_t>(m_mem2_size / PAGE_SIZE));

  // After loading, the target slot becomes the new "oldest" slot,
  // so the next save will overwrite the next slot.
  m_ring_next  = Wrap(target_slot + 1, NUM_SAVE_SLOTS);
  m_ring_count = m_ring_count - frames_back;

  if (ok)
  {
    INFO_LOG_FMT(BRAWLBACK, "Rolled back {} frame(s)", frames_back);
  }
  else
    OSD::AddMessage("Rollback state load failed", 3000, OSD::Color::RED);
}

}  // namespace Rollback
