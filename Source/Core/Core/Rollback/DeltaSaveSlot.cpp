
#include "Core/Rollback/DeltaSaveSlot.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <span>
#include <intrin.h>
#include <immintrin.h>

#include "Core/Rollback/Perf.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/State.h"
#include "Core/System.h"
#include <Common/Assert.h>


namespace Rollback
{

// "nt" = Non Temporal. Bypasses cpu cache and goes into a write-combine buffer which is ideal for large memcpys like savestates
// i found 1 level of loop unrolling like this was best at least for my cpu
__forceinline static void copy_avx2_nt_unroll(void* __restrict dst, const void* __restrict src,
                                              size_t n)
{
  const char* s = static_cast<const char*>(src);
  char* d = static_cast<char*>(dst);
  size_t i = 0;
  for (; i + 256 <= n; i += 256)
  {
    __m256i v0 = _mm256_loadu_si256((const __m256i*)(s + i + 0));
    __m256i v1 = _mm256_loadu_si256((const __m256i*)(s + i + 32));
    __m256i v2 = _mm256_loadu_si256((const __m256i*)(s + i + 64));
    __m256i v3 = _mm256_loadu_si256((const __m256i*)(s + i + 96));
    __m256i v4 = _mm256_loadu_si256((const __m256i*)(s + i + 128));
    __m256i v5 = _mm256_loadu_si256((const __m256i*)(s + i + 160));
    __m256i v6 = _mm256_loadu_si256((const __m256i*)(s + i + 192));
    __m256i v7 = _mm256_loadu_si256((const __m256i*)(s + i + 224));
    _mm256_stream_si256((__m256i*)(d + i + 0), v0);
    _mm256_stream_si256((__m256i*)(d + i + 32), v1);
    _mm256_stream_si256((__m256i*)(d + i + 64), v2);
    _mm256_stream_si256((__m256i*)(d + i + 96), v3);
    _mm256_stream_si256((__m256i*)(d + i + 128), v4);
    _mm256_stream_si256((__m256i*)(d + i + 160), v5);
    _mm256_stream_si256((__m256i*)(d + i + 192), v6);
    _mm256_stream_si256((__m256i*)(d + i + 224), v7);
  }
  _mm_sfence();
  if (i < n)
    std::memcpy(d + i, s + i, n - i);
}

// memcpy but uses faster wide non-temporal stores, and skips subranges that are in our exclusion list
// TODO: profile this, if it's slow maybe we can sort the exclusion list beforehand
void savestateMemcpy(void* dst, const void* src, size_t size,
                     uint32_t dst_phys,
                     const std::vector<ExcludeRegion>& excl)
{
  if (excl.empty())
  {
    copy_avx2_nt_unroll(dst, src, size);
    return;
  }

  auto* d       = static_cast<uint8_t*>(dst);
  const auto* s = static_cast<const uint8_t*>(src);

  uint32_t cursor = dst_phys;
  const uint32_t end = dst_phys + static_cast<uint32_t>(size);

  while (cursor < end)
  {
    // If cursor falls inside an excluded region, jump past it.
    bool jumped = false;
    for (const auto& r : excl)
    {
      if (cursor >= r.phys_start && cursor < r.phys_end)
      {
        cursor = std::min(r.phys_end, end);
        jumped = true;
        break;
      }
    }
    if (jumped)
      continue;

    // Find the nearest upcoming exclusion boundary within [cursor, end).
    uint32_t seg_end = end;
    for (const auto& r : excl)
    {
      if (r.phys_start > cursor && r.phys_start < seg_end)
        seg_end = r.phys_start;
    }

    // Copy the clear segment.
    const size_t offset    = cursor - dst_phys;
    const size_t copy_size = seg_end - cursor;
    copy_avx2_nt_unroll(d + offset, s + offset, copy_size);
    cursor = seg_end;
  }
}

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

// region_phys_base: Wii physical base of the region
//   MEM1 -> 0,          MEM2 -> 0x10000000
static void RestoreRegionDelta(const RegionDelta& delta, uint8_t* region_base,
                               uint32_t region_phys_base,
                               const std::vector<ExcludeRegion>& excl)
{
  for (uint32_t i = 0; i < delta.page_count; ++i)
  {
    const uint32_t page_idx  = delta.page_indices[i];
    uint8_t* const dst       = region_base + static_cast<size_t>(page_idx) * PAGE_SIZE;
    const uint8_t* const src = delta.page_data.data() + static_cast<size_t>(i) * PAGE_SIZE;
    const uint32_t dst_phys  = region_phys_base + page_idx * static_cast<uint32_t>(PAGE_SIZE);
    savestateMemcpy(dst, src, PAGE_SIZE, dst_phys, excl);
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

void DeltaSaveSlot::ApplyDeltaReverse(const std::vector<ExcludeRegion>& excl) const
{
  ROLLBACK_ZONE();

  ASSERT(m_has_state);
  ASSERT(m_mem1_ptr);

  RestoreRegionDelta(m_mem1_delta, m_mem1_ptr, 0u, excl);
  if (m_mem2_ptr && m_mem2_page_count > 0)
    RestoreRegionDelta(m_mem2_delta, m_mem2_ptr, 0x10000000u, excl);
}

}  // namespace Rollback
