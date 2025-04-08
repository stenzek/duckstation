// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "memory_scanner.h"
#include "bus.h"
#include "cpu_core.h"
#include "cpu_core_private.h"

#include "common/log.h"

#include "fmt/format.h"

LOG_CHANNEL(Cheats);

static bool IsValidScanAddress(VirtualMemoryAddress address)
{
  if ((address & CPU::SCRATCHPAD_ADDR_MASK) == CPU::SCRATCHPAD_ADDR &&
      (address & CPU::SCRATCHPAD_OFFSET_MASK) < CPU::SCRATCHPAD_SIZE)
  {
    return true;
  }

  const PhysicalMemoryAddress phys_address = CPU::VirtualAddressToPhysical(address);
  if (phys_address < Bus::RAM_MIRROR_END)
    return true;

  if (phys_address >= Bus::BIOS_BASE && phys_address < (Bus::BIOS_BASE + Bus::BIOS_SIZE))
    return true;

  return false;
}

MemoryScan::MemoryScan() = default;

MemoryScan::~MemoryScan() = default;

void MemoryScan::ResetSearch()
{
  m_results.clear();
}

void MemoryScan::Search()
{
  m_results.clear();

  switch (m_size)
  {
    case MemoryAccessSize::Byte:
      SearchBytes();
      break;

    case MemoryAccessSize::HalfWord:
      SearchHalfwords();
      break;

    case MemoryAccessSize::Word:
      SearchWords();
      break;

    default:
      break;
  }
}

void MemoryScan::SearchBytes()
{
  for (PhysicalMemoryAddress address = m_start_address; address < m_end_address; address++)
  {
    if (!IsValidScanAddress(address))
      continue;

    u8 bvalue = 0;
    if (!CPU::SafeReadMemoryByte(address, &bvalue)) [[unlikely]]
      continue;

    Result res;
    res.address = address;
    res.value = m_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    res.last_value = res.value;
    res.value_changed = false;

    if (res.Filter(m_operator, m_value, m_signed))
      m_results.push_back(res);
  }
}

void MemoryScan::SearchHalfwords()
{
  for (PhysicalMemoryAddress address = m_start_address; address < m_end_address; address += 2)
  {
    if (!IsValidScanAddress(address))
      continue;

    u16 bvalue = 0;
    if (!CPU::SafeReadMemoryHalfWord(address, &bvalue)) [[unlikely]]
      continue;

    Result res;
    res.address = address;
    res.value = m_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    res.last_value = res.value;
    res.value_changed = false;

    if (res.Filter(m_operator, m_value, m_signed))
      m_results.push_back(res);
  }
}

void MemoryScan::SearchWords()
{
  for (PhysicalMemoryAddress address = m_start_address; address < m_end_address; address += 4)
  {
    if (!IsValidScanAddress(address))
      continue;

    u32 bvalue = 0;
    if (!CPU::SafeReadMemoryWord(address, &bvalue)) [[unlikely]]
      continue;

    Result res;
    res.address = address;
    res.value = bvalue;
    res.last_value = res.value;
    res.value_changed = false;

    if (res.Filter(m_operator, m_value, m_signed))
      m_results.push_back(res);
  }
}

void MemoryScan::SearchAgain()
{
  ResultVector new_results;
  new_results.reserve(m_results.size());
  for (Result& res : m_results)
  {
    res.UpdateValue(m_size, m_signed);

    if (res.Filter(m_operator, m_value, m_signed))
    {
      res.last_value = res.value;
      new_results.push_back(res);
    }
  }

  m_results.swap(new_results);
}

void MemoryScan::UpdateResultsValues()
{
  for (Result& res : m_results)
    res.UpdateValue(m_size, m_signed);
}

void MemoryScan::SetResultValue(u32 index, u32 value)
{
  if (index >= m_results.size())
    return;

  Result& res = m_results[index];
  if (res.value == value)
    return;

  switch (m_size)
  {
    case MemoryAccessSize::Byte:
      CPU::SafeWriteMemoryByte(res.address, Truncate8(value));
      break;

    case MemoryAccessSize::HalfWord:
      CPU::SafeWriteMemoryHalfWord(res.address, Truncate16(value));
      break;

    case MemoryAccessSize::Word:
      CPU::SafeWriteMemoryWord(res.address, value);
      break;
  }

  res.value = value;
  res.value_changed = true;
}

bool MemoryScan::Result::Filter(Operator op, u32 comp_value, bool is_signed) const
{
  switch (op)
  {
    case Operator::Equal:
    {
      return (value == comp_value);
    }

    case Operator::NotEqual:
    {
      return (value != comp_value);
    }

    case Operator::GreaterThan:
    {
      return is_signed ? (static_cast<s32>(value) > static_cast<s32>(comp_value)) : (value > comp_value);
    }

    case Operator::GreaterEqual:
    {
      return is_signed ? (static_cast<s32>(value) >= static_cast<s32>(comp_value)) : (value >= comp_value);
    }

    case Operator::LessThan:
    {
      return is_signed ? (static_cast<s32>(value) < static_cast<s32>(comp_value)) : (value < comp_value);
    }

    case Operator::LessEqual:
    {
      return is_signed ? (static_cast<s32>(value) <= static_cast<s32>(comp_value)) : (value <= comp_value);
    }

    case Operator::IncreasedBy:
    {
      return is_signed ? ((static_cast<s32>(value) - static_cast<s32>(last_value)) == static_cast<s32>(comp_value)) :
                         ((value - last_value) == comp_value);
    }

    case Operator::DecreasedBy:
    {
      return is_signed ? ((static_cast<s32>(last_value) - static_cast<s32>(value)) == static_cast<s32>(comp_value)) :
                         ((last_value - value) == comp_value);
    }

    case Operator::ChangedBy:
    {
      if (is_signed)
        return (std::abs(static_cast<s32>(last_value) - static_cast<s32>(value)) == static_cast<s32>(comp_value));
      else
        return ((last_value > value) ? (last_value - value) : (value - last_value)) == comp_value;
    }

    case Operator::EqualLast:
    {
      return (value == last_value);
    }

    case Operator::NotEqualLast:
    {
      return (value != last_value);
    }

    case Operator::GreaterThanLast:
    {
      return is_signed ? (static_cast<s32>(value) > static_cast<s32>(last_value)) : (value > last_value);
    }

    case Operator::GreaterEqualLast:
    {
      return is_signed ? (static_cast<s32>(value) >= static_cast<s32>(last_value)) : (value >= last_value);
    }

    case Operator::LessThanLast:
    {
      return is_signed ? (static_cast<s32>(value) < static_cast<s32>(last_value)) : (value < last_value);
    }

    case Operator::LessEqualLast:
    {
      return is_signed ? (static_cast<s32>(value) <= static_cast<s32>(last_value)) : (value <= last_value);
    }

    case Operator::Any:
      return true;

    default:
      return false;
  }
}

void MemoryScan::Result::UpdateValue(MemoryAccessSize size, bool is_signed)
{
  const u32 old_value = value;

  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      u8 bvalue = 0;
      if (CPU::SafeReadMemoryByte(address, &bvalue)) [[likely]]
        value = is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::HalfWord:
    {
      u16 bvalue = 0;
      if (CPU::SafeReadMemoryHalfWord(address, &bvalue)) [[likely]]
        value = is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::Word:
    {
      CPU::SafeReadMemoryWord(address, &value);
    }
    break;
  }

  value_changed = (value != old_value);
}

MemoryWatchList::MemoryWatchList() = default;

MemoryWatchList::~MemoryWatchList() = default;

const MemoryWatchList::Entry* MemoryWatchList::GetEntryByAddress(u32 address) const
{
  for (const Entry& entry : m_entries)
  {
    if (entry.address == address)
      return &entry;
  }

  return nullptr;
}

bool MemoryWatchList::AddEntry(std::string description, u32 address, MemoryAccessSize size, bool is_signed, bool freeze)
{
  if (GetEntryByAddress(address))
    return false;

  Entry entry;
  entry.description = std::move(description);
  entry.address = address;
  entry.size = size;
  entry.is_signed = is_signed;
  entry.freeze = false;

  UpdateEntryValue(&entry);

  entry.changed = false;
  entry.freeze = freeze;

  m_entries.push_back(std::move(entry));
  return true;
}

bool MemoryWatchList::GetEntryFreeze(u32 index) const
{
  if (index >= m_entries.size())
    return false;

  return m_entries[index].freeze;
}

void MemoryWatchList::RemoveEntry(u32 index)
{
  if (index >= m_entries.size())
    return;

  m_entries.erase(m_entries.begin() + index);
}

bool MemoryWatchList::RemoveEntryByAddress(u32 address)
{
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
  {
    if (it->address == address)
    {
      m_entries.erase(it);
      return true;
    }
  }

  return false;
}

void MemoryWatchList::SetEntryDescription(u32 index, std::string description)
{
  if (index >= m_entries.size())
    return;

  Entry& entry = m_entries[index];
  entry.description = std::move(description);
}

void MemoryWatchList::SetEntryFreeze(u32 index, bool freeze)
{
  if (index >= m_entries.size())
    return;

  Entry& entry = m_entries[index];
  entry.freeze = freeze;
}

void MemoryWatchList::SetEntryValue(u32 index, u32 value)
{
  if (index >= m_entries.size())
    return;

  Entry& entry = m_entries[index];
  if (entry.value == value)
    return;

  SetEntryValue(&entry, value);
}

bool MemoryWatchList::RemoveEntryByDescription(const char* description)
{
  bool result = false;
  for (auto it = m_entries.begin(); it != m_entries.end();)
  {
    if (it->description == description)
    {
      it = m_entries.erase(it);
      result = true;
      continue;
    }

    ++it;
  }

  return result;
}

void MemoryWatchList::UpdateValues()
{
  for (Entry& entry : m_entries)
    UpdateEntryValue(&entry);
}

void MemoryWatchList::SetEntryValue(Entry* entry, u32 value)
{
  switch (entry->size)
  {
    case MemoryAccessSize::Byte:
      CPU::SafeWriteMemoryByte(entry->address, Truncate8(value));
      break;

    case MemoryAccessSize::HalfWord:
      CPU::SafeWriteMemoryHalfWord(entry->address, Truncate16(value));
      break;

    case MemoryAccessSize::Word:
      CPU::SafeWriteMemoryWord(entry->address, value);
      break;
  }

  entry->changed = (entry->value != value);
  entry->value = value;
}

void MemoryWatchList::UpdateEntryValue(Entry* entry)
{
  const u32 old_value = entry->value;

  switch (entry->size)
  {
    case MemoryAccessSize::Byte:
    {
      u8 bvalue = 0;
      if (CPU::SafeReadMemoryByte(entry->address, &bvalue)) [[likely]]
        entry->value = entry->is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::HalfWord:
    {
      u16 bvalue = 0;
      if (CPU::SafeReadMemoryHalfWord(entry->address, &bvalue)) [[likely]]
        entry->value = entry->is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::Word:
    {
      CPU::SafeReadMemoryWord(entry->address, &entry->value);
    }
    break;
  }

  entry->changed = (old_value != entry->value);

  if (entry->freeze && entry->changed)
    SetEntryValue(entry, old_value);
}
