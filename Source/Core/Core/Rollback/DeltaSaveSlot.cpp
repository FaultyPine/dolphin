
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
                     const std::vector<MemoryRegion>& excl)
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
  brawl_frame = 0;
  m_mem1_delta.Reset();
  m_mem2_delta.Reset();
  m_save_buffer.reset();
}

// region_phys_base: Wii physical base of the region
//   MEM1 -> 0,          MEM2 -> 0x10000000
void RestoreRegionDelta(const RegionDelta& delta, uint8_t* region_base,
                               uint32_t region_phys_base,
                               const std::vector<MemoryRegion>& excl)
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

void enqueueSubsectionJobs(u32 first_page, u32 page_count, const uint8_t* region_base,
                           Rollback::RegionDelta& out, JITDirtyBitmap& bitmap,
                           job::JobTaskThread& w, job::Job& root, std::vector<job::Job*>& jar, u32 num_work_chunks)
{
  const uint8_t* entries = bitmap.entries;
  auto dirty_pages = std::make_shared<std::vector<u32>>();
  {
    ROLLBACK_ZONE_N("dirty page population");
    dirty_pages->reserve(page_count);
    for (uint32_t i = 0; i < page_count; ++i)
    {
      if (entries[first_page + i])
      {
        dirty_pages->push_back(i + first_page);
      }
    }
  }

  u32 num_dirty = static_cast<u32>(dirty_pages->size());
  if (!num_dirty)
    return;
  out.page_count = num_dirty;
  {
    ROLLBACK_ZONE_N("delta buffer alloc");
    if (out.page_indices.size() < num_dirty)
      out.page_indices.reset(num_dirty);
    if (out.page_data.size() < (num_dirty * PAGE_SIZE))
      out.page_data.reset(num_dirty * PAGE_SIZE);
  }

  // Capture pointers to the already-allocated buffers to avoid reference capture issues
  uint16_t* page_indices_ptr = out.page_indices.data();
  uint8_t* page_data_ptr = out.page_data.data();

  auto copySubsectionFn = [dirty_pages, entries, region_base, page_indices_ptr, page_data_ptr, first_page](u32 offset, u32 size) {
    ROLLBACK_ZONE_N("copy dirty pages");
    if (offset >= dirty_pages->size())
      return;  // Nothing to do for this subsection

    u32 this_split_written = 0;
    for (uint32_t dirtyPagesIndex = offset;
         dirtyPagesIndex < offset + size && dirtyPagesIndex < dirty_pages->size();
         ++dirtyPagesIndex)
    {
      u32 pageidx = (*dirty_pages)[dirtyPagesIndex];
      ASSERT(entries[pageidx]);

      // pageidx is global; subtract first_page to get region-relative index
      u32 relative_page_idx = pageidx - first_page;
      page_indices_ptr[dirtyPagesIndex] = static_cast<uint16_t>(relative_page_idx);
      std::memcpy(page_data_ptr + static_cast<size_t>(dirtyPagesIndex) * PAGE_SIZE,
                  region_base + static_cast<size_t>(relative_page_idx) * PAGE_SIZE, PAGE_SIZE);
      ++this_split_written;
    }
#if defined(HAVE_TRACY)
    auto x =
        StringFromFormat("page count %u (offset %u size %u)", this_split_written, offset, size);
    ZoneText(x.c_str(), x.size());
#endif
  };

  u32 split_size = (num_dirty + num_work_chunks - 1) / num_work_chunks;
  for (u32 i = 0; i < num_work_chunks; ++i)
  {
    u32 offset = i * split_size;
    u32 size = std::min(split_size, num_dirty - offset);
    jar.push_back(w.create_job_as_child(root, [copySubsectionFn, offset, size](job::JobTaskThread& t, job::Job& j) {
      copySubsectionFn(offset, size);
    }));
  }
}


void DeltaSaveSlot::Save(Core::System& system)
{
  ROLLBACK_ZONE();
  ASSERT(m_mem1_ptr);
  ASSERT(m_mem1_page_count < MEM2_FIRST_PAGE);

  brawl_frame = ReadBrawlMatchFrameCounter(m_mem2_ptr, m_mem2_size);
  auto& bitmap = JITDirtyBitmap::Get();
  auto& rbm    = RollbackManager::Get();
  auto* dt     = rbm.m_dispatch_thread;
  ASSERT(dt);
  
  job::Job* l1cachejob = job::KickRootJob(dt, [this](job::JobTaskThread&, job::Job&) {
    ROLLBACK_ZONE_N("L1 cache save");
    Rollback::DeltaSaveSlot* slot = this;
    if (slot->m_l1_cache_ptr && slot->m_l1_cache_size > 0 && slot->m_l1_cache_snapshot.data())
      std::memcpy(slot->m_l1_cache_snapshot.data(), slot->m_l1_cache_ptr, slot->m_l1_cache_size);
  });

  job::Job* root_mem1_job = job::KickRootJob(dt, [this, &bitmap](job::JobTaskThread& w, job::Job& root) {
    ROLLBACK_ZONE_N("root mem1 save dispatch");
    std::vector<job::Job*> jar;
    enqueueSubsectionJobs(0, m_mem1_page_count, m_mem1_ptr, m_mem1_delta, bitmap, w, root, jar, SAVESTATE_NUM_WORK_CHUNKS);
    auto num_jobs = jar.size();
    ASSERT(num_jobs <= UINT16_MAX);
    w.do_work_and_kick_jobs(jar.data(), (uint16_t)num_jobs);
  });

  // do all mem1 copy jobs + l1 cache job. THEN do mem2 jobs.
  // goal here is to try not to trash the cache too hard
  job::Job* root_mem2_job = job::KickRootJob(dt, [this, &bitmap](job::JobTaskThread& w, job::Job& root) {
    ROLLBACK_ZONE_N("root mem2 save dispatch");
    std::vector<job::Job*> jar;
    enqueueSubsectionJobs(MEM2_FIRST_PAGE, m_mem2_page_count, m_mem2_ptr, m_mem2_delta, bitmap, w, root, jar, SAVESTATE_NUM_WORK_CHUNKS);
    auto num_jobs = jar.size();
    ASSERT(num_jobs <= UINT16_MAX);
    w.do_work_and_kick_jobs(jar.data(), (uint16_t)num_jobs);
  });

  // Run DoState on this thread while workers capture pages.
  {
    ROLLBACK_ZONE_N("DoState save");
    rbm.BeginDoState();
    State::SaveToBuffer(system, m_save_buffer);
    rbm.EndDoState();
  }

  job::DrainJobsUntilComplete(dt, root_mem1_job);
  job::DrainJobsUntilComplete(dt, root_mem2_job);
  job::DrainJobsUntilComplete(dt, l1cachejob);

  bitmap.ClearRange(0, m_mem1_page_count);
  bitmap.ClearRange(MEM2_FIRST_PAGE, m_mem2_page_count);
  m_has_state = true;
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

}  // namespace Rollback
