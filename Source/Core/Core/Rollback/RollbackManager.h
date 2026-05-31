// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <bitset>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "Core/Rollback/DeltaSaveSlot.h"
#include "Core/Brawlback/BrawlbackUtility.h"
#include "job.h"

// Set to 1 to enable full-RAM shadow snapshots for rollback validation
#define ROLLBACK_VALIDATE 0

namespace Core
{
class System;
}

namespace Rollback
{
static constexpr int NUM_SAVE_SLOTS = MAX_ROLLBACK_FRAMES + 1;
static constexpr int ROLLBACK_NUM_HELPER_THREADS = 4;
class RollbackManager
{
public:

  RollbackManager() = default;
  ~RollbackManager() = default;

  RollbackManager(const RollbackManager&) = delete;
  RollbackManager& operator=(const RollbackManager&) = delete;
  RollbackManager(RollbackManager&&) = delete;
  RollbackManager& operator=(RollbackManager&&) = delete;

  static RollbackManager& Get();

  void Init(Core::System& system);
  void Shutdown();

  void SaveFrame(Core::System& system);
  void LoadFrame(Core::System& system, int frames_back = 1);

  void AddExcludeRegion(uint32_t virt_addr, uint32_t size_bytes);

  bool IsInitialized() const { return m_initialized; }

  void ToggleFrameSave();

  alignas(64) std::atomic<bool> m_frame_save_enabled{false};
  alignas(64) std::atomic<bool> m_frame_save_pending{false};

  void BeginDoState();
  void EndDoState();

  alignas(64) std::atomic<bool> m_skip_ram_in_dostate{false};
  alignas(64) std::atomic<bool> m_skip_ios_in_dostate{false};
  alignas(64) std::atomic<bool> m_skip_jit_clear_in_dostate{false};

  void NotifyDBATMappingsWereUpdated() {}

  // WSQ job system: m_dispatch_thread is worker 0, owned by the rollback thread.
  // Background workers run wait_for_termination() on their own std::threads.
  job::JobSysCtx       m_job_ctx;
  job::JobTaskThread*  m_dispatch_thread = nullptr;
  std::vector<std::thread> m_worker_threads;

  bool m_initialized = false;

  uint8_t* m_mem1_ptr  = nullptr;
  size_t   m_mem1_size = 0;
  uint8_t* m_mem2_ptr  = nullptr;
  size_t   m_mem2_size = 0;
  uint8_t* m_l1_cache_ptr  = nullptr;
  size_t   m_l1_cache_size = 0;

  DeltaSaveSlot m_slots[NUM_SAVE_SLOTS];

  std::vector<ExcludeRegion> m_exclude_regions;

  int m_ring_next  = 0;  // index of the slot that will be written by the next savestate
  int m_ring_count = 0;

  struct RollbackSnapshot
  {
    std::unique_ptr<uint8_t[]> mem1;
    std::unique_ptr<uint8_t[]> mem2;
    std::shared_mutex mutex;
    uint32_t brawl_frame = 0;
    bool valid = false;
  };

  // Rolling base: full MEM1+MEM2 state at the oldest reachable frame
  RollbackSnapshot m_base_snapshot;

  job::Job* m_eviction_job = nullptr;

  void CaptureFullRamSnapshot(RollbackSnapshot& snap);

#if ROLLBACK_VALIDATE
  RollbackSnapshot m_val_snapshots[NUM_SAVE_SLOTS];

  void CompareValSnapshot(int target_slot, int frames_back) const;
  void InvalidateValSnapshots();
#endif
};

uint32_t ReadBrawlFrameCounter(const uint8_t* mem2_ptr, size_t mem2_size);

}  // namespace Rollback
