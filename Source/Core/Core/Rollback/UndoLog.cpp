// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/UndoLog.h"

#include "Common/Profiler.h"

#include <algorithm>
#include <cstring>

namespace Rollback
{
namespace
{
constexpr std::size_t BLOCK_PAGES = 64;
}

UndoLog::UndoLog(std::size_t page_size, std::size_t page_count, u64 first_id)
    : m_page_size(page_size), m_page_count(page_count), m_next_id(std::max<u64>(first_id, 1)),
      m_recorded_in(page_count, 0), m_recorded_data(page_count, nullptr), m_scratch(page_count, 0)
{
}

std::deque<UndoLog::Log>::iterator UndoLog::FindLog(u64 id)
{
  return std::find_if(m_logs.begin(), m_logs.end(), [id](const Log& log) { return log.id == id; });
}

std::deque<UndoLog::Log>::const_iterator UndoLog::FindLog(u64 id) const
{
  return std::find_if(m_logs.begin(), m_logs.end(), [id](const Log& log) { return log.id == id; });
}

u32 UndoLog::NextMark() const
{
  if (++m_mark == 0)
  {
    // Wrapped: clear so no stale mark equals a new one.
    std::fill(m_scratch.begin(), m_scratch.end(), 0);
    m_mark = 1;
  }
  return m_mark;
}

u64 UndoLog::Open()
{
  PROFILER_SCOPE_FUNCTION();
  std::vector<Entry> entries;
  if (!m_spare_entry_storage.empty())
  {
    entries = std::move(m_spare_entry_storage.back());
    m_spare_entry_storage.pop_back();
  }
  // Reserve entry storage so committing dirty pages rarely allocates at a frame boundary.
  entries.reserve(std::max<std::size_t>(1024, m_page_count / 8));
  m_logs.push_back(Log{m_next_id++, std::move(entries)});
  return m_logs.back().id;
}

bool UndoLog::Has(u64 id) const
{
  return id != 0 && FindLog(id) != m_logs.end();
}

bool UndoLog::LoggedInNewest(std::size_t page) const
{
  return !m_logs.empty() && m_recorded_in[page] == m_logs.back().id;
}

bool UndoLog::Record(std::size_t page, const u8* contents)
{
  PROFILER_SCOPE_FUNCTION();
  if (m_logs.empty() || LoggedInNewest(page))
    return false;
  u8* const data = TakeBuffer();
  std::memcpy(data, contents, m_page_size);
  Log& newest = m_logs.back();
  newest.entries.push_back(Entry{static_cast<u32>(page), data});
  m_recorded_in[page] = newest.id;
  m_recorded_data[page] = data;
  ++m_pages_held;
  return true;
}

std::vector<u32> UndoLog::NewestPages() const
{
  PROFILER_SCOPE_FUNCTION();
  std::vector<u32> pages;
  if (m_logs.empty())
    return pages;
  pages.reserve(m_logs.back().entries.size());
  for (const Entry& entry : m_logs.back().entries)
    pages.push_back(entry.page);
  return pages;
}

bool UndoLog::RewindTo(u64 id)
{
  PROFILER_SCOPE_FUNCTION();
  const auto target = FindLog(id);
  if (target == m_logs.end())
    return false;
  while (m_logs.back().id != id)
  {
    FreeEntries(&m_logs.back());
    RecycleEntryStorage(&m_logs.back());
    m_logs.pop_back();
  }
  // Pages from the dropped logs carry ids that no longer match the newest. Reset the emptied log's
  // pages so they stop matching too.
  for (const Entry& entry : target->entries)
    m_recorded_in[entry.page] = 0;
  FreeEntries(&*target);
  return true;
}

void UndoLog::Drop(u64 id)
{
  PROFILER_SCOPE_FUNCTION();
  const auto log = FindLog(id);
  if (log == m_logs.end())
    return;
  if (log == m_logs.begin())
  {
    FreeEntries(&*log);
    RecycleEntryStorage(&*log);
    m_logs.pop_front();
    return;
  }
  const auto older = std::prev(log);
  const u32 mark = NextMark();
  for (const Entry& entry : older->entries)
    m_scratch[entry.page] = mark;
  for (const Entry& entry : log->entries)
  {
    if (m_scratch[entry.page] == mark)
    {
      // The older log already has what the page held back then.
      m_free.push_back(entry.data);
      --m_pages_held;
    }
    else
    {
      older->entries.push_back(entry);
    }
  }
  log->entries.clear();
  RecycleEntryStorage(&*log);
  const bool was_newest = std::next(log) == m_logs.end();
  m_logs.erase(log);
  if (was_newest)
  {
    // The older log is now the newest, so its pages are the ones that need no new record.
    const u64 newest = m_logs.back().id;
    for (const Entry& entry : m_logs.back().entries)
    {
      m_recorded_in[entry.page] = newest;
      m_recorded_data[entry.page] = entry.data;
    }
  }
}

void UndoLog::Clear()
{
  PROFILER_SCOPE_FUNCTION();
  for (Log& log : m_logs)
    FreeEntries(&log);
  m_logs.clear();
  std::fill(m_recorded_in.begin(), m_recorded_in.end(), 0);
}

void UndoLog::Reserve(std::size_t pages)
{
  PROFILER_SCOPE_FUNCTION();
  while (m_free.size() < pages)
  {
    m_blocks.push_back(std::make_unique<u8[]>(BLOCK_PAGES * m_page_size));
    u8* const block = m_blocks.back().get();
    for (std::size_t i = 0; i < BLOCK_PAGES; ++i)
      m_free.push_back(block + i * m_page_size);
  }
}

u8* UndoLog::TakeBuffer()
{
  PROFILER_SCOPE_FUNCTION();
  if (m_free.empty())
    Reserve(1);
  u8* const data = m_free.back();
  m_free.pop_back();
  return data;
}

void UndoLog::FreeEntries(Log* log)
{
  PROFILER_SCOPE_FUNCTION();
  for (const Entry& entry : log->entries)
    m_free.push_back(entry.data);
  m_pages_held -= log->entries.size();
  log->entries.clear();
}

void UndoLog::RecycleEntryStorage(Log* log)
{
  PROFILER_SCOPE_FUNCTION();
  if (log->entries.capacity() != 0)
    m_spare_entry_storage.push_back(std::move(log->entries));
}
}  // namespace Rollback
