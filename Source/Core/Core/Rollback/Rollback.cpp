// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Rollback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <tuple>
#include <unordered_map>

#include "Common/FPURoundMode.h"
#include "Common/Logging/Log.h"
#include "Common/Profiler.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/DirtyBitmap.h"
#include "Core/Rollback/DirtyPages.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{
namespace
{
std::atomic<bool> s_in_snapshot{false};
std::atomic<bool> s_resimulating{false};
std::atomic<u64> s_resim_jit_compile_blocks{0};
std::atomic<u64> s_resim_jit_compile_nanoseconds{0};
std::atomic<u64> s_rollback_burst_sequence{0};
std::atomic<u64> s_rollback_burst_serial{0};
std::atomic<u64> s_rollback_burst_end_time_ns{0};
std::atomic<u64> s_rollback_burst_duration_ns{0};
ResimEventStats s_resim_event_stats;
std::unordered_map<u32, ResimGuestHotspot> s_resim_guest_hotspots;
ResimJitMemoryStats s_resim_jit_memory_stats;

constexpr std::size_t RESTORE_PAGE = 4096;
constexpr std::size_t JIT_CACHE_LINE = 32;
constexpr u32 MEM1_VIRTUAL = 0x80000000u;
constexpr u32 MEM2_VIRTUAL = 0x90000000u;
// Effective to physical for MEM1/MEM2 and their uncached mirrors (0x8/0xC -> 0x0, 0x9/0xD -> 0x1).
constexpr u32 PHYSICAL_MASK = 0x1FFFFFFFu;

double MillisecondsSince(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Copies saved RAM back one page at a time, skipping pages that already match. Only changed pages
// have their JIT blocks invalidated; clearing the whole JIT (as a normal state load does) would
// recompile the hot code after every rollback. Changed pages are collected by physical address.
void RestoreRam(u8* live, const std::vector<u8>& saved, u32 virtual_base,
                std::vector<u32>* changed_pages, std::vector<ChangedMemoryRange>* changed_ranges)
{
  PROFILER_SCOPE_FUNCTION();
  const std::size_t size = saved.size();
  for (std::size_t offset = 0; offset < size; offset += RESTORE_PAGE)
  {
    const std::size_t length = std::min(RESTORE_PAGE, size - offset);
    if (std::memcmp(live + offset, saved.data() + offset, length) == 0)
      continue;
    const u32 physical = (virtual_base + static_cast<u32>(offset)) & PHYSICAL_MASK;
    for (std::size_t line = 0; line < length; line += JIT_CACHE_LINE)
    {
      const std::size_t line_length = std::min(JIT_CACHE_LINE, length - line);
      if (std::memcmp(live + offset + line, saved.data() + offset + line, line_length) != 0)
      {
        changed_ranges->push_back(
            {physical + static_cast<u32>(line), static_cast<u32>(line_length)});
      }
    }
    std::memcpy(live + offset, saved.data() + offset, length);
    changed_pages->push_back(physical);
  }
}

// The JIT also remembers per-instruction facts (GPU FIFO writes, paired quantization, no
// speculative constants). A restored page may hold different instructions, so forget its entries.
// Done here in bulk because the JIT's own per-word invalidation is too slow for thousands of pages.
void ForgetLearnedJitAddresses(Core::System& system, const std::vector<u32>& changed_pages)
{
  PROFILER_SCOPE_FUNCTION();
  // JitInterface only holds a JitBase (null for the interpreter). static_cast because MSVC builds
  // have RTTI off.
  auto* jit = static_cast<JitBase*>(system.GetJitInterface().GetCore());
  if (!jit || changed_pages.empty())
    return;
  const auto in_changed_page = [&changed_pages](u32 address) {
    const u32 page = address & PHYSICAL_MASK & ~static_cast<u32>(RESTORE_PAGE - 1);
    return std::binary_search(changed_pages.begin(), changed_pages.end(), page);
  };
  std::erase_if(jit->js.fifoWriteAddresses, in_changed_page);
  std::erase_if(jit->js.pairedQuantizeAddresses, in_changed_page);
  std::erase_if(jit->js.noSpeculativeConstantsAddresses, in_changed_page);
}

// The NAND file system, which is always the host backend. Its journal is part of Orca's
// authoritative whole-emulator rollback state.
IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}
}  // namespace

bool InSnapshotDoState()
{
  return s_in_snapshot.load(std::memory_order_relaxed);
}

bool IsResimulating()
{
  return s_resimulating.load(std::memory_order_relaxed);
}

void SetResimulating(bool resimulating)
{
  s_resimulating.store(resimulating, std::memory_order_relaxed);
  VideoCommon_SetSkipRender(resimulating);
}

void RecordRollbackBurst(u64 duration_ns)
{
  const u64 end_time_ns = static_cast<u64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  s_rollback_burst_sequence.fetch_add(1, std::memory_order_acq_rel);
  s_rollback_burst_duration_ns.store(duration_ns, std::memory_order_relaxed);
  s_rollback_burst_end_time_ns.store(end_time_ns, std::memory_order_relaxed);
  s_rollback_burst_serial.fetch_add(1, std::memory_order_relaxed);
  s_rollback_burst_sequence.fetch_add(1, std::memory_order_release);
}

RollbackBurstMarker GetLastRollbackBurst()
{
  RollbackBurstMarker marker;
  while (true)
  {
    const u64 sequence_before = s_rollback_burst_sequence.load(std::memory_order_acquire);
    if ((sequence_before & 1) != 0)
      continue;
    marker.serial = s_rollback_burst_serial.load(std::memory_order_relaxed);
    marker.end_time_ns = s_rollback_burst_end_time_ns.load(std::memory_order_relaxed);
    marker.duration_ns = s_rollback_burst_duration_ns.load(std::memory_order_relaxed);
    const u64 sequence_after = s_rollback_burst_sequence.load(std::memory_order_acquire);
    if (sequence_before == sequence_after)
      return marker;
  }
}

void RecordResimJitCompile(u64 nanoseconds)
{
  s_resim_jit_compile_blocks.fetch_add(1, std::memory_order_relaxed);
  s_resim_jit_compile_nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
}

ResimJitCompileStats GetResimJitCompileStats()
{
  return {s_resim_jit_compile_blocks.load(std::memory_order_relaxed),
          s_resim_jit_compile_nanoseconds.load(std::memory_order_relaxed)};
}

void RecordResimGuestSlice(u32 pc, u64 nanoseconds, u64 cycles)
{
  auto [it, inserted] = s_resim_guest_hotspots.try_emplace(pc);
  ResimGuestHotspot& hotspot = it->second;
  if (inserted)
    hotspot.pc = pc;
  hotspot.nanoseconds += nanoseconds;
  hotspot.cycles += cycles;
  ++hotspot.slices;
}

std::vector<ResimGuestHotspot> TakeResimGuestHotspots(std::size_t limit)
{
  std::vector<ResimGuestHotspot> hotspots;
  hotspots.reserve(s_resim_guest_hotspots.size());
  for (const auto& [pc, hotspot] : s_resim_guest_hotspots)
    hotspots.push_back(hotspot);
  s_resim_guest_hotspots.clear();

  std::ranges::sort(hotspots, std::greater{}, &ResimGuestHotspot::nanoseconds);
  if (hotspots.size() > limit)
    hotspots.resize(limit);
  return hotspots;
}

void RecordResimJitMemoryHelper(bool write, u64 nanoseconds)
{
  if (write)
  {
    ++s_resim_jit_memory_stats.write_calls;
    s_resim_jit_memory_stats.write_nanoseconds += nanoseconds;
  }
  else
  {
    ++s_resim_jit_memory_stats.read_calls;
    s_resim_jit_memory_stats.read_nanoseconds += nanoseconds;
  }
}

ResimJitMemoryStats TakeResimJitMemoryStats()
{
  return std::exchange(s_resim_jit_memory_stats, {});
}

void RecordResimEvent(ResimEventCategory category, u64 nanoseconds)
{
  const std::size_t index = static_cast<std::size_t>(category);
  s_resim_event_stats.nanoseconds[index] += nanoseconds;
  ++s_resim_event_stats.callbacks[index];
}

ResimEventStats GetResimEventStats()
{
  return s_resim_event_stats;
}

SnapshotScope::SnapshotScope()
{
  s_in_snapshot.store(true, std::memory_order_relaxed);
  // In a session, host GPU state (EFB, texture cache, bounding box) never reaches emulated RAM, so
  // snapshots skip it: no GPU readback on save, no texture-cache reload on load.
  VideoCommon_SetRollbackSnapshot(true);
}

SnapshotScope::~SnapshotScope()
{
  VideoCommon_SetRollbackSnapshot(false);
  s_in_snapshot.store(false, std::memory_order_relaxed);
}

SnapshotRing::SnapshotRing(std::size_t slots, bool tracked_bitmap_clear, bool force_full_scan)
    : m_slots(std::max<std::size_t>(slots, 2)), m_tracked_bitmap_clear(tracked_bitmap_clear),
      m_force_full_scan(force_full_scan)
{
  m_changed_blocks.reserve(4096);
  m_changed_ranges.reserve(32768);
}

SnapshotRing::~SnapshotRing()
{
  DirtyPages::Disarm(this);
}

void SnapshotRing::Forget(Slot* slot)
{
  PROFILER_SCOPE_FUNCTION();
  if (slot->page_snapshot_id != 0)
    DirtyPages::Drop(slot->page_snapshot_id);
  slot->page_snapshot_id = 0;
  slot->frame = -1;
}

bool SnapshotRing::UseDirtyPageTracking(Core::System& system, bool* armed_now)
{
  PROFILER_SCOPE_FUNCTION();
  *armed_now = false;
  // If tracking stopped under us (emulation stopped, or an unsupported RAM mapping), the
  // undo-log snapshots are gone.
  if (m_dirty_page_tracking.value_or(false) && !DirtyPages::IsArmedFor(this))
  {
    for (Slot& slot : m_slots)
    {
      if (slot.page_snapshot_id != 0)
      {
        slot.page_snapshot_id = 0;
        slot.frame = -1;
      }
    }
    m_dirty_page_tracking.reset();
  }
  if (!m_dirty_page_tracking)
  {
    m_dirty_page_tracking = DirtyPages::ArmForSystem(system, this, m_force_full_scan);
    *armed_now = *m_dirty_page_tracking;
  }
  return *m_dirty_page_tracking;
}

SnapshotRing::Slot* SnapshotRing::Find(s64 frame)
{
  for (Slot& slot : m_slots)
  {
    if (slot.frame == frame)
      return &slot;
  }
  return nullptr;
}

const SnapshotRing::Slot* SnapshotRing::Find(s64 frame) const
{
  for (const Slot& slot : m_slots)
  {
    if (slot.frame == frame)
      return &slot;
  }
  return nullptr;
}

bool SnapshotRing::Has(s64 frame) const
{
  const Slot* slot = Find(frame);
  return slot && (slot->page_snapshot_id == 0 || DirtyPages::Has(slot->page_snapshot_id));
}

std::span<const u8> SnapshotRing::LastState() const
{
  const Slot& slot = m_slots[m_last];
  return {slot.state.data(), slot.state_size};
}

const State::RollbackStateLayout& SnapshotRing::LastStateLayout() const
{
  return m_slots[m_last].state_layout;
}

std::span<const u8> SnapshotRing::StateForFrame(s64 frame) const
{
  const Slot* slot = Find(frame);
  return slot ? std::span<const u8>{slot->state.data(), slot->state_size} : std::span<const u8>{};
}

void SnapshotRing::Reset(Core::System& system)
{
  PROFILER_SCOPE_FUNCTION();
  for (Slot& slot : m_slots)
  {
    slot.frame = -1;
    slot.page_snapshot_id = 0;
  }
  DirtyPages::Disarm(this);
  m_dirty_page_tracking.reset();
  m_next = 0;
  m_initial_trace_events = 0;
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
    nand->JournalStop();
}

bool SnapshotRing::Save(Core::System& system, s64 frame)
{
  PROFILER_SCOPE_FUNCTION();
  const bool trace = m_initial_trace_events++ < 16;
  if (trace)
    NOTICE_LOG_FMT(CORE, "Rollback trace: save frame {} begin", frame);
  m_last_save_timings = {};
  // Re-saving a frame after a rewind reuses its slot; otherwise take the next one.
  Slot* slot = Find(frame);
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  m_last = static_cast<std::size_t>(slot - m_slots.data());
  Forget(slot);
  bool armed_now;
  const bool dirty_page_tracking = UseDirtyPageTracking(system, &armed_now);
  if (trace)
    NOTICE_LOG_FMT(CORE, "Rollback trace: save frame {} tracking ready (dirty={}, armed={})",
                   frame, dirty_page_tracking, armed_now);
  const auto buffers = [slot] {
    return std::tuple{slot->state.data(), slot->mem1.capacity(), slot->mem2.capacity()};
  };
  const auto buffers_before = buffers();

  // Non-RAM state first, so anything DoState writes into RAM is in the RAM snapshot.
  {
    const auto phase_start = std::chrono::steady_clock::now();
    SnapshotScope scope;
    slot->state_size =
        State::SaveToBufferForRollback(system, slot->state, &slot->state_layout);
    if (trace)
      NOTICE_LOG_FMT(CORE, "Rollback trace: save frame {} state done ({} bytes)", frame,
                     slot->state_size);
    m_last_save_timings.state_ms = MillisecondsSince(phase_start);
    m_last_save_timings.state_bytes = slot->state_size;
  }

  auto& memory = system.GetMemory();
  // Dirty-page tracking: commit the RAM written since the last snapshot and open this snapshot's
  // undo log. A 0 id means tracking stopped, so fall back to a full copy.
  if (dirty_page_tracking)
  {
    const auto phase_start = std::chrono::steady_clock::now();
    slot->page_snapshot_id = DirtyPages::Snapshot();
    if (trace)
      NOTICE_LOG_FMT(CORE, "Rollback trace: save frame {} dirty commit done (id={})", frame,
                     slot->page_snapshot_id);
    m_last_save_timings.dirty_pages_ms = MillisecondsSince(phase_start);
  }
  if (slot->page_snapshot_id == 0)
  {
    const auto phase_start = std::chrono::steady_clock::now();
    slot->mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
    if (memory.GetEXRAM())
      slot->mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
    else
      slot->mem2.clear();
    m_last_save_timings.dirty_pages_ms = MillisecondsSince(phase_start);
  }
  const auto l1_start = std::chrono::steady_clock::now();
  slot->l1_cache.assign(memory.GetL1Cache(), memory.GetL1Cache() + memory.GetL1CacheSize());
  m_last_save_timings.l1_ms = MillisecondsSince(l1_start);
  slot->frame = frame;

  // NAND changes older than the oldest snapshot can never be undone, so trim them.
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
  {
    const auto phase_start = std::chrono::steady_clock::now();
    slot->nand_journal_mark = nand->LastJournalMark();
    u64 oldest = slot->nand_journal_mark;
    for (const Slot& other : m_slots)
    {
      if (other.frame >= 0)
        oldest = std::min(oldest, other.nand_journal_mark);
    }
    nand->JournalTrim(oldest);
    m_last_save_timings.journal_ms = MillisecondsSince(phase_start);
  }

  if (trace)
    NOTICE_LOG_FMT(CORE, "Rollback trace: save frame {} complete", frame);

  // Arming allocates and initializes the RAM mirrors; that one-off cost counts as an allocation.
  return armed_now || buffers() != buffers_before;
}

bool SnapshotRing::Load(Core::System& system, s64 frame)
{
  PROFILER_SCOPE_FUNCTION();
  return LoadSlot(system, frame, false);
}

std::optional<u64> SnapshotRing::NandJournalMarkForFrame(s64 frame) const
{
  const Slot* slot = Find(frame);
  if (!slot)
    return std::nullopt;
  return slot->nand_journal_mark;
}

bool SnapshotRing::LoadSlot(Core::System& system, s64 frame, bool redisplay)
{
  PROFILER_SCOPE_FUNCTION();
  const bool trace = m_initial_trace_events++ < 16;
  if (trace)
    NOTICE_LOG_FMT(CORE, "Rollback trace: load frame {} begin", frame);
  m_last_load_timings = {};
  Slot* slot = Find(frame);
  if (!slot || slot->state_size == 0)
  {
    ERROR_LOG_FMT(CORE, "Rollback: no snapshot for frame {}", frame);
    return false;
  }

  // Restore RAM before device state: some devices (emulated Bluetooth) re-parse pending IOS
  // requests from RAM while loading, and live RAM may already hold a newer reply. The device-state
  // load writes no RAM in a session, since EFB copies stay on the GPU.
  auto& memory = system.GetMemory();
  const auto dirty_start = std::chrono::steady_clock::now();
  m_changed_blocks.clear();
  m_changed_ranges.clear();
  if (slot->page_snapshot_id != 0)
  {
    // The undo logs of this snapshot and every newer one list exactly the pages written since.
    if (!DirtyPages::Restore(
            slot->page_snapshot_id,
            [this](u32 physical, u32 length) {
              m_changed_ranges.push_back({physical, length});
              const u32 page = physical & ~static_cast<u32>(RESTORE_PAGE - 1);
              if (m_changed_blocks.empty() || m_changed_blocks.back() != page)
                m_changed_blocks.push_back(page);
            },
            m_tracked_bitmap_clear))
    {
      ERROR_LOG_FMT(CORE, "Rollback: the dirty-page snapshot for frame {} is gone", frame);
      return false;
    }
    if (trace)
      NOTICE_LOG_FMT(CORE, "Rollback trace: load frame {} dirty restore done", frame);
  }
  else
  {
    RestoreRam(memory.GetRAM(), slot->mem1, MEM1_VIRTUAL, &m_changed_blocks, &m_changed_ranges);
    if (memory.GetEXRAM() && !slot->mem2.empty())
      RestoreRam(memory.GetEXRAM(), slot->mem2, MEM2_VIRTUAL, &m_changed_blocks,
                 &m_changed_ranges);
  }
  std::sort(m_changed_blocks.begin(), m_changed_blocks.end());
  m_changed_blocks.erase(std::unique(m_changed_blocks.begin(), m_changed_blocks.end()),
                         m_changed_blocks.end());
  m_last_load_timings.dirty_pages_ms = MillisecondsSince(dirty_start);
  m_last_load_timings.changed_blocks = m_changed_blocks.size();
  m_last_load_timings.changed_cache_lines = m_changed_ranges.size();
  // Standalone ARAM is not CPU executable memory. FakeVMEM's canonical bitmap range maps back to
  // its physical fastmem address for invalidation.
  auto& jit = system.GetJitInterface();
  const std::size_t jit_blocks_before = jit.GetBlockCount();
  const auto jit_start = std::chrono::steady_clock::now();
  for (const ChangedMemoryRange& range : m_changed_ranges)
  {
    const u32 physical = range.physical_address;
    if (physical >= DirtyPages::GC_ARAM_PHYSICAL &&
        physical < DirtyPages::GC_ARAM_PHYSICAL + DSP::ARAM_SIZE)
      continue;
    const bool fake_vmem = physical >= DirtyPages::GC_FAKE_VMEM_PHYSICAL &&
                           physical < DirtyPages::GC_FAKE_VMEM_PHYSICAL +
                                          memory.GetFakeVMemSize();
    const u32 effective = fake_vmem ? physical | 0x60000000u : physical | MEM1_VIRTUAL;
    jit.InvalidateICache(effective, range.length, true);
  }
  ForgetLearnedJitAddresses(system, m_changed_blocks);
  m_last_load_timings.jit_ms = MillisecondsSince(jit_start);
  const std::size_t jit_blocks_after = jit.GetBlockCount();
  m_last_load_timings.jit_blocks_invalidated =
      jit_blocks_before > jit_blocks_after ? jit_blocks_before - jit_blocks_after : 0;
  const auto l1_start = std::chrono::steady_clock::now();
  std::memcpy(memory.GetL1Cache(), slot->l1_cache.data(), slot->l1_cache.size());
  m_last_load_timings.l1_ms = MillisecondsSince(l1_start);

  bool ok;
  {
    const auto phase_start = std::chrono::steady_clock::now();
    SnapshotScope scope;
    VideoCommon_SetSnapshotRedisplays(redisplay);
    ok = State::LoadFromBufferForRollback(system,
                                          std::span<u8>(slot->state.data(), slot->state_size));
    if (trace)
      NOTICE_LOG_FMT(CORE, "Rollback trace: load frame {} state done (ok={})", frame, ok);
    VideoCommon_SetSnapshotRedisplays(false);
    m_last_load_timings.state_ms = MillisecondsSince(phase_start);
    m_last_load_timings.state_bytes = slot->state_size;
  }
  if (!ok)
  {
    ERROR_LOG_FMT(CORE, "Rollback: snapshot for frame {} did not load", frame);
    return false;
  }
  // The NAND journal must have undone every change since this snapshot.
  const auto journal_start = std::chrono::steady_clock::now();
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system); nand && nand->TakeJournalError())
  {
    ERROR_LOG_FMT(CORE, "Rollback: the NAND could not be restored to frame {}", frame);
    return false;
  }
  m_last_load_timings.journal_ms = MillisecondsSince(journal_start);

  if (trace)
    NOTICE_LOG_FMT(CORE, "Rollback trace: load frame {} complete", frame);

  // Later frames are about to be re-run and re-saved, so drop their snapshots.
  for (Slot& other : m_slots)
  {
    if (other.frame > frame ||
        (other.page_snapshot_id != 0 && !DirtyPages::Has(other.page_snapshot_id)))
      Forget(&other);
  }
  m_next = (static_cast<std::size_t>(slot - m_slots.data()) + 1) % m_slots.size();
  return true;
}

bool SnapshotRing::Capture(Core::System& system, MachineImage* image, bool stop_journal)
{
  PROFILER_SCOPE_FUNCTION();
  Common::UniqueBuffer<u8> state;
  std::size_t state_size;
  {
    SnapshotScope scope;
    state_size = State::SaveToBufferForRollback(system, state);
  }
  if (state_size == 0)
    return false;
  image->state.assign(state.data(), state.data() + state_size);
  auto& memory = system.GetMemory();
  image->mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
  if (memory.GetEXRAM())
    image->mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
  else
    image->mem2.clear();
  image->l1_cache.assign(memory.GetL1Cache(), memory.GetL1Cache() + memory.GetL1CacheSize());
  if (stop_journal)
  {
    if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
      nand->JournalStop();
  }
  return true;
}

bool SnapshotRing::LoadImage(Core::System& system, MachineImage image, s64 frame)
{
  PROFILER_SCOPE_FUNCTION();
  auto& memory = system.GetMemory();
  if (image.state.empty() || image.mem1.size() != memory.GetRamSize() ||
      image.mem2.size() != (memory.GetEXRAM() ? memory.GetExRamSize() : 0) ||
      image.l1_cache.size() != memory.GetL1CacheSize())
  {
    ERROR_LOG_FMT(CORE, "Rollback: keyframe for frame {} doesn't fit this machine", frame);
    return false;
  }
  Slot* slot = Find(frame);
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  // Store as a full-copy slot (page_snapshot_id 0), so LoadSlot restores mem1/mem2 directly.
  Forget(slot);
  slot->state.reset(image.state.size());
  std::memcpy(slot->state.data(), image.state.data(), image.state.size());
  slot->state_size = image.state.size();
  slot->mem1 = std::move(image.mem1);
  slot->mem2 = std::move(image.mem2);
  slot->l1_cache = std::move(image.l1_cache);
  slot->frame = frame;
  slot->nand_journal_mark = 0;
  // A joining machine has nothing on screen yet, so show the keyframe's frame.
  return LoadSlot(system, frame, true);
}

}  // namespace Rollback
