// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Undo logs for dirty-page rollback snapshots (DirtyPages.h). Each snapshot's log holds the old
// contents ("pre-images") of pages first written after it was taken.
//
// Logs are kept oldest first; log i covers writes between snapshot i and snapshot i + 1. To rebuild
// memory at snapshot s, take live memory and replace each page found in logs s..newest with its
// pre-image from the oldest of those logs. Pages in none of them have not changed since s.
//
// Pure bookkeeping with no platform code: the caller reports first writes and restores pages
// itself. Not thread-safe; DirtyPages.cpp holds its lock around every call.

#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Profiler.h"

namespace Rollback
{
class UndoLog
{
public:
  // Ids start at `first_id` (nonzero) so a new log can continue an earlier one's numbering.
  UndoLog(std::size_t page_size, std::size_t page_count, u64 first_id = 1);
  UndoLog(const UndoLog&) = delete;
  UndoLog& operator=(const UndoLog&) = delete;

  std::size_t PageSize() const { return m_page_size; }
  std::size_t PageCount() const { return m_page_count; }

  // Opens an empty log for a new snapshot, which becomes the newest. Returns its id; ids only grow
  // and are never 0.
  u64 Open();
  bool Has(u64 id) const;
  // The newest snapshot's id, or 0 when there is none.
  u64 Newest() const { return m_logs.empty() ? 0 : m_logs.back().id; }
  std::size_t SnapshotCount() const { return m_logs.size(); }
  // The id the next Open returns.
  u64 NextId() const { return m_next_id; }

  // Whether `page` already has a pre-image in the newest log, so a write to it needs no record.
  bool LoggedInNewest(std::size_t page) const;
  // That pre-image, or null.
  const u8* NewestPreImage(std::size_t page) const
  {
    return LoggedInNewest(page) ? m_recorded_data[page] : nullptr;
  }
  // Records `contents` (PageSize() bytes) as `page`'s pre-image in the newest log, unless already
  // there or there is no snapshot. Returns whether it recorded.
  bool Record(std::size_t page, const u8* contents);
  // Pages in the newest log, in first-write order.
  std::vector<u32> NewestPages() const;

  // Calls f(page, pre_image) once per page changed since snapshot `id`, with the page's contents at
  // `id`. Does nothing if `id` is not held.
  template <typename F>
  void ForEachPreImage(u64 id, F&& f) const
  {
    PROFILER_SCOPE_FUNCTION();
    const auto first = FindLog(id);
    if (first == m_logs.end())
      return;
    const u32 mark = NextMark();
    for (auto log = first; log != m_logs.end(); ++log)
    {
      for (const Entry& entry : log->entries)
      {
        if (m_scratch[entry.page] == mark)
          continue;
        m_scratch[entry.page] = mark;
        f(static_cast<std::size_t>(entry.page), static_cast<const u8*>(entry.data));
      }
    }
  }

  // Call after memory is restored to snapshot `id`: drops newer logs and empties `id`'s, which is
  // the newest again. Returns false and changes nothing if `id` is not held.
  bool RewindTo(u64 id);
  // Forgets snapshot `id`. Pre-images the next older log lacks move into it (a page unwritten
  // between the two snapshots held the same bytes at both). The oldest log is simply dropped.
  void Drop(u64 id);
  void Clear();

  // Pre-images held across all logs.
  std::size_t PagesHeld() const { return m_pages_held; }
  // Keeps at least `pages` free buffers so recording dirty pages at a boundary rarely allocates.
  void Reserve(std::size_t pages);

private:
  struct Entry
  {
    u32 page;
    u8* data;
  };
  struct Log
  {
    u64 id;
    std::vector<Entry> entries;
  };

  std::deque<Log>::iterator FindLog(u64 id);
  std::deque<Log>::const_iterator FindLog(u64 id) const;
  u32 NextMark() const;
  u8* TakeBuffer();
  void FreeEntries(Log* log);
  void RecycleEntryStorage(Log* log);

  std::size_t m_page_size;
  std::size_t m_page_count;
  std::deque<Log> m_logs;
  // Dropped logs donate their entry vectors to later snapshots. This keeps the per-frame Open()
  // path from allocating a fresh multi-thousand-entry vector.
  std::vector<std::vector<Entry>> m_spare_entry_storage;
  u64 m_next_id = 1;
  // Per page: the id of the newest log that recorded it, and that pre-image.
  std::vector<u64> m_recorded_in;
  std::vector<u8*> m_recorded_data;
  // Per-page marks for deduplicating one pass over several logs.
  mutable std::vector<u32> m_scratch;
  mutable u32 m_mark = 0;
  std::size_t m_pages_held = 0;
  // Page buffers come from blocks of BLOCK_PAGES, recycled through the free list.
  std::vector<std::unique_ptr<u8[]>> m_blocks;
  std::vector<u8*> m_free;
};
}  // namespace Rollback
