
#pragma once

#ifdef _WIN32

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Common/Buffer.h"
#include "Core/Rollback/IRollbackSaveSlot.h"

static constexpr size_t PAGE_SIZE = 4096;

namespace Rollback
{

// One byte per 4KB physical page in the Wii fastmem arena (0x00000000–0x1FFFFFFF).
// JIT stores write 1 to entries[phys_addr >> 12] 
// Plain global so jit can embed its address as an absolute immediate (see EmitJITDirtyBitmapUpdate)
struct alignas(64) JITDirtyBitmap
{
  static constexpr size_t ARENA_SIZE  = 512ULL * 1024 * 1024;  // 512 MB
  static constexpr size_t ENTRY_COUNT = ARENA_SIZE / PAGE_SIZE;

  uint8_t entries[ENTRY_COUNT];

  static JITDirtyBitmap& Get()
  {
    static JITDirtyBitmap s_instance;
    return s_instance;
  }

  void Clear() { std::memset(entries, 0, sizeof(entries)); }
  void ClearRange(uint32_t first_page, uint32_t page_count)
  {
    std::memset(&entries[first_page], 0, page_count);
  }
};

struct RegionDelta
{
  uint32_t page_count = 0;
  Common::UniqueBuffer<uint16_t> page_indices;  // [page_count] relative page indices
  Common::UniqueBuffer<uint8_t>  page_data;     // [page_count * PAGE_SIZE] saved page contents

  void Reset()
  {
    page_count = 0;
    page_indices.reset();
    page_data.reset();
  }
};

struct EvictedDelta
{
  RegionDelta mem1;
  RegionDelta mem2;
};

class DeltaSaveSlot final : public IRollbackSaveSlot
{
public:
  static constexpr uint32_t MEM2_FIRST_PAGE = 0x10000000u / 4096u;

  void Init(uint8_t* mem1_ptr, size_t mem1_size,
            uint8_t* mem2_ptr, size_t mem2_size,
            uint8_t* l1_cache_ptr, size_t l1_cache_size);

  bool HasState() const override { return m_has_state; }
  void Reset() override;
  void Save(Core::System& system) override;
  void Load(Core::System& system) override {}

  void ApplyDeltaReverse() const;
  bool RestoreNonDeltaState(Core::System& system);
  EvictedDelta ExtractDeltas();
  void MarkTouchedGlobalPages(std::bitset<JITDirtyBitmap::ENTRY_COUNT>& touched) const;

private:
  uint8_t* m_mem1_ptr      = nullptr;
  size_t   m_mem1_size     = 0;
  uint8_t* m_mem2_ptr      = nullptr;
  size_t   m_mem2_size     = 0;
  uint8_t* m_l1_cache_ptr  = nullptr;
  size_t   m_l1_cache_size = 0;

  uint32_t m_mem1_page_count = 0;
  uint32_t m_mem2_page_count = 0;

  RegionDelta m_mem1_delta;
  RegionDelta m_mem2_delta;

  Common::UniqueBuffer<uint8_t> m_l1_cache_snapshot;  // outside fastmem arena, not bitmap-tracked
  Common::UniqueBuffer<uint8_t> m_save_buffer;         // non-RAM state (CPU, HW, IOS, etc.)

  bool m_has_state = false;
};

}  // namespace Rollback

#endif  // _WIN32
