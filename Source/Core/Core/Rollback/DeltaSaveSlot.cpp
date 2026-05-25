
#include "Core/Rollback/DeltaSaveSlot.h"

#include <cassert>
#include <cstring>
#include <span>

#include "Core/Rollback/Perf.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/State.h"
#include "Core/System.h"
#include <Common/Assert.h>

namespace Rollback
{

void DeltaSaveSlot::Init(uint8_t* mem1_ptr, size_t mem1_size,
                          uint8_t* mem2_ptr, size_t mem2_size,
                          uint8_t* l1_cache_ptr, size_t l1_cache_size)
{
  m_mem1_ptr        = mem1_ptr;
  m_mem1_size       = mem1_size;
  m_mem2_ptr        = mem2_ptr;
  m_mem2_size       = mem2_size;
  m_l1_cache_ptr    = l1_cache_ptr;
  m_l1_cache_size   = l1_cache_size;
  m_mem1_page_count = static_cast<uint32_t>(mem1_size / PAGE_SIZE);
  m_mem2_page_count = static_cast<uint32_t>(mem2_size / PAGE_SIZE);

  if (l1_cache_ptr && l1_cache_size > 0)
    m_l1_cache_snapshot.reset(l1_cache_size);

  m_has_state = false;
}

void DeltaSaveSlot::Reset()
{
  m_has_state = false;
  m_mem1_delta.Reset();
  m_mem2_delta.Reset();
  m_save_buffer.reset();
}

static void CaptureRegionDelta(RegionDelta& out, const uint8_t* entries,
                                uint32_t first_page, uint32_t page_count,
                                const uint8_t* region_base)
{
  uint32_t dirty_count = 0;
  for (uint32_t i = 0; i < page_count; ++i)
    if (entries[first_page + i]) ++dirty_count;

  out.page_count = dirty_count;
  out.page_indices.reset(dirty_count);
  out.page_data.reset(static_cast<size_t>(dirty_count) * PAGE_SIZE);

  uint32_t written = 0;
  for (uint32_t i = 0; i < page_count; ++i)
  {
    if (!entries[first_page + i])
      continue;

    out.page_indices[written] = static_cast<uint16_t>(i);
    std::memcpy(out.page_data.data() + static_cast<size_t>(written) * PAGE_SIZE,
                region_base          + static_cast<size_t>(i)        * PAGE_SIZE,
                PAGE_SIZE);
    ++written;
  }
}

static void RestoreRegionDelta(const RegionDelta& delta, uint8_t* region_base)
{
  for (uint32_t i = 0; i < delta.page_count; ++i)
  {
    std::memcpy(region_base                               + static_cast<size_t>(delta.page_indices[i]) * PAGE_SIZE,
                delta.page_data.data()                    + static_cast<size_t>(i)                     * PAGE_SIZE,
                PAGE_SIZE);
  }
}

void DeltaSaveSlot::Save(Core::System& system)
{
  ROLLBACK_ZONE();

  ASSERT(m_mem1_ptr);

  auto& bitmap = JITDirtyBitmap::Get();

  CaptureRegionDelta(m_mem1_delta, bitmap.entries, 0, m_mem1_page_count, m_mem1_ptr);
  if (m_mem2_ptr && m_mem2_page_count > 0)
    CaptureRegionDelta(m_mem2_delta, bitmap.entries, MEM2_FIRST_PAGE, m_mem2_page_count, m_mem2_ptr);

  // no longer dirty now that all page data has been captured
  bitmap.ClearRange(0, m_mem1_page_count);
  if (m_mem2_ptr && m_mem2_page_count > 0)
    bitmap.ClearRange(MEM2_FIRST_PAGE, m_mem2_page_count);

  // L1 cache is outside JIT fastmem arena, not tracked by dirty bitmap
  if (m_l1_cache_ptr && m_l1_cache_size > 0 && m_l1_cache_snapshot.data())
    std::memcpy(m_l1_cache_snapshot.data(), m_l1_cache_ptr, m_l1_cache_size);

  auto& mgr = RollbackManager::Get();
  mgr.BeginDoState();
  State::SaveToBuffer(system, m_save_buffer);
  mgr.EndDoState();

  m_has_state = true;
}

bool DeltaSaveSlot::RestoreNonDeltaState(Core::System& system)
{
  ROLLBACK_ZONE();

  ASSERT(m_has_state);

  if (m_l1_cache_ptr && m_l1_cache_size > 0 && m_l1_cache_snapshot.data())
    std::memcpy(m_l1_cache_ptr, m_l1_cache_snapshot.data(), m_l1_cache_size);

  auto& mgr = RollbackManager::Get();
  mgr.BeginDoState();
  const bool ok = State::LoadFromBuffer(
      system, std::span<uint8_t>(m_save_buffer.data(), m_save_buffer.size()));
  mgr.EndDoState();

  return ok;
}

EvictedDelta DeltaSaveSlot::ExtractDeltas()
{
  ROLLBACK_ZONE();
  EvictedDelta out;
  out.mem1 = std::move(m_mem1_delta);
  out.mem2 = std::move(m_mem2_delta);
  m_has_state = false;
  return out;
}

void DeltaSaveSlot::MarkTouchedGlobalPages(std::bitset<JITDirtyBitmap::ENTRY_COUNT>& touched) const
{
  ROLLBACK_ZONE();
  for (uint32_t i = 0; i < m_mem1_delta.page_count; ++i)
    touched.set(m_mem1_delta.page_indices[i]);
  for (uint32_t i = 0; i < m_mem2_delta.page_count; ++i)
    touched.set(MEM2_FIRST_PAGE + m_mem2_delta.page_indices[i]);
}

void DeltaSaveSlot::ApplyDeltaReverse() const
{
  ROLLBACK_ZONE();

  ASSERT(m_has_state);
  ASSERT(m_mem1_ptr);

  RestoreRegionDelta(m_mem1_delta, m_mem1_ptr);
  if (m_mem2_ptr && m_mem2_page_count > 0)
    RestoreRegionDelta(m_mem2_delta, m_mem2_ptr);
}

}  // namespace Rollback
