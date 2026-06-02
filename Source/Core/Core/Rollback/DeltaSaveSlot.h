
#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"
#include "Core/Rollback/IRollbackSaveSlot.h"

static constexpr size_t PAGE_SIZE = 4096;
static constexpr uint32_t MEM2_BASE = 0x10000000u;
static constexpr uint32_t MEM2_FIRST_PAGE = MEM2_BASE / 4096u;

namespace Rollback
{

// Specified as a Wii virtual address, stored internally as physical offsets
struct MemoryRegion
{
  uint32_t phys_start = 0;  // virt_addr & 0x1FFF'FFFFu
  uint32_t phys_end = 0;    // phys_start + size

  static MemoryRegion FromVirt(uint32_t virt_addr, uint32_t size_bytes)
  {
    const uint32_t phys = virt_addr & 0x1FFF'FFFFu;
    return {phys, phys + size_bytes};
  }
};

struct MemoryRegionThroughPtrs : public MemoryRegion
{
  uint32_t base_virt_addr;
  std::vector<uint32_t> pointer_offsets;
  uint32_t final_data_size;

  static MemoryRegionThroughPtrs FromVirt(uint32_t virt_addr, uint32_t size_bytes)
  {
    const uint32_t phys = virt_addr & 0x1FFF'FFFFu;
    return {phys, phys + size_bytes};
  }

  static MemoryRegionThroughPtrs FromPtrs(uint32_t virt_addr,
                                          std::initializer_list<uint32_t> offsets,
                                          uint32_t data_size)
  {
    const uint32_t phys = virt_addr & 0x1FFF'FFFFu;
    MemoryRegionThroughPtrs r{};
    r.phys_start = phys;
    r.phys_end = phys;
    r.base_virt_addr = virt_addr;
    r.pointer_offsets = offsets;
    r.final_data_size = data_size;
    return r;
  }

  static uint32_t ResolvePointer(uint32_t phys_addr, uint8_t* mem1_ptr, size_t mem1_size,
                                 uint8_t* mem2_ptr, size_t mem2_size)
  {
    const uint32_t phys_masked = phys_addr & 0x1FFF'FFFFu;
    u32 value = 0;

    if (phys_masked < mem1_size)
    {
      std::memcpy(&value, mem1_ptr + phys_masked, sizeof(u32));
      // swap32
      return (value >> 24) | ((value & 0xFF0000) >> 8) | ((value & 0xFF00) << 8) | (value << 24);
    }
    else if (phys_masked >= MEM2_BASE && phys_masked - MEM2_BASE < mem2_size)
    {
      std::memcpy(&value, mem2_ptr + (phys_masked - MEM2_BASE), sizeof(u32));
      // swap32
      return (value >> 24) | ((value & 0xFF0000) >> 8) | ((value & 0xFF00) << 8) | (value << 24);
    }

    return 0;  // Out of bounds
  }

  MemoryRegion Resolve(uint8_t* mem1_ptr, size_t mem1_size, uint8_t* mem2_ptr,
                       size_t mem2_size) const
  {
    uint32_t current_phys = base_virt_addr & 0x1FFF'FFFFu;

    // Follow the pointer chain
    for (int i = 0; i < pointer_offsets.size(); ++i)
    {
      uint32_t deref = ResolvePointer(current_phys, mem1_ptr, mem1_size, mem2_ptr, mem2_size);
      if (deref == 0)
        return {0, 0};

      current_phys = (deref + pointer_offsets[i]) & 0x1FFF'FFFFu;
    }

    return MemoryRegion::FromVirt(current_phys, final_data_size);
  }
};


void savestateMemcpy(void* dst, const void* src, size_t size,
                     uint32_t dst_phys,
                     const std::vector<MemoryRegion>& exclude_regions);

}  // namespace Rollback

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

  void Init(uint8_t* mem1_ptr, size_t mem1_size,
            uint8_t* mem2_ptr, size_t mem2_size,
            uint8_t* l1_cache_ptr, size_t l1_cache_size);

  bool HasState() const override { return m_has_state; }
  void Reset() override;
  void Save(Core::System& system) override;
  void Load(Core::System& system) override {}

  EvictedDelta ExtractDeltas();
  void MarkTouchedGlobalPages(std::bitset<JITDirtyBitmap::ENTRY_COUNT>& touched) const;

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

  uint32_t brawl_frame = 0;

  Common::UniqueBuffer<uint8_t> m_l1_cache_snapshot;  // outside fastmem arena, not bitmap-tracked
  Common::UniqueBuffer<uint8_t> m_save_buffer;         // non-RAM state (CPU, HW, IOS, etc.)

  bool m_has_state = false;
};

void RestoreRegionDelta(const RegionDelta& delta, uint8_t* region_base, uint32_t region_phys_base, const std::vector<MemoryRegion>& excl);

}  // namespace Rollback
