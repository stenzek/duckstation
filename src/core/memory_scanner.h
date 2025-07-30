// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class MemoryScan
{
public:
  enum class Operator
  {
    Any,
    LessThanLast,
    LessEqualLast,
    GreaterThanLast,
    GreaterEqualLast,
    NotEqualLast,
    EqualLast,
    DecreasedBy,
    IncreasedBy,
    ChangedBy,
    Equal,
    NotEqual,
    LessThan,
    LessEqual,
    GreaterThan,
    GreaterEqual
  };

  struct Result
  {
    PhysicalMemoryAddress address;
    u32 value;
    u32 last_value;
    u32 first_value;
    bool value_changed;

    bool Filter(Operator op, u32 comp_value, bool is_signed) const;
    void UpdateValue(MemoryAccessSize size, bool is_signed);
  };

  using ResultVector = std::vector<Result>;

  MemoryScan();
  ~MemoryScan();

  u32 GetValue() const { return m_value; }
  bool GetValueSigned() const { return m_signed; }
  MemoryAccessSize GetSize() const { return m_size; }
  Operator GetOperator() const { return m_operator; }
  PhysicalMemoryAddress GetStartAddress() const { return m_start_address; }
  PhysicalMemoryAddress GetEndAddress() const { return m_end_address; }
  const ResultVector& GetResults() const { return m_results; }
  const Result& GetResult(u32 index) const { return m_results[index]; }
  u32 GetResultCount() const { return static_cast<u32>(m_results.size()); }

  void SetValue(u32 value) { m_value = value; }
  void SetValueSigned(bool s) { m_signed = s; }
  void SetSize(MemoryAccessSize size) { m_size = size; }
  void SetOperator(Operator op) { m_operator = op; }
  void SetStartAddress(PhysicalMemoryAddress addr) { m_start_address = addr; }
  void SetEndAddress(PhysicalMemoryAddress addr) { m_end_address = addr; }

  void ResetSearch();
  void Search();
  void SearchAgain();
  void UpdateResultsValues();

  void SetResultValue(u32 index, u32 value);

private:
  void SearchBytes();
  void SearchHalfwords();
  void SearchWords();

  u32 m_value = 0;
  MemoryAccessSize m_size = MemoryAccessSize::HalfWord;
  Operator m_operator = Operator::Any;
  PhysicalMemoryAddress m_start_address = 0;
  PhysicalMemoryAddress m_end_address = 0x200000;
  ResultVector m_results;
  bool m_signed = false;
};

class MemoryWatchList
{
public:
  MemoryWatchList();
  ~MemoryWatchList();

  struct Entry
  {
    std::string description;
    u32 address;
    u32 value;
    MemoryAccessSize size;
    bool is_signed;
    bool freeze;
    bool changed;
  };

  using EntryVector = std::vector<Entry>;

  const Entry* GetEntryByAddress(u32 address) const;
  const EntryVector& GetEntries() const { return m_entries; }
  const Entry& GetEntry(u32 index) const { return m_entries[index]; }
  u32 GetEntryCount() const { return static_cast<u32>(m_entries.size()); }

  bool AddEntry(std::string description, u32 address, MemoryAccessSize size, bool is_signed, bool freeze);
  bool GetEntryFreeze(u32 index) const;

  void RemoveEntry(u32 index);
  bool RemoveEntryByDescription(const char* description);
  bool RemoveEntryByAddress(u32 address);

  void SetEntryDescription(u32 index, std::string description);
  void SetEntryFreeze(u32 index, bool freeze);
  void SetEntryValue(u32 index, u32 value);

  void UpdateValues();

private:
  static void SetEntryValue(Entry* entry, u32 value);
  static void UpdateEntryValue(Entry* entry);

  EntryVector m_entries;
};
