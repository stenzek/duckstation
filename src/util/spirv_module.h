// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <iterator>
#include <optional>
#include <span>
#include <spirv.hpp>

class Error;

// Helper class for range-based iteration
class SPIRVModule
{
public:
  static constexpr size_t HEADER_SIZE = 5;

  class SPIRVInstructionIterator
  {
  public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = u32;
    using difference_type = std::ptrdiff_t;
    using pointer = u32*;
    using const_pointer = const u32*;
    using reference = u32&;

  private:
    std::span<u32> m_module;
    size_t m_offset;

  public:
    // Constructors
    SPIRVInstructionIterator();

    SPIRVInstructionIterator(std::span<u32> module, size_t offset);

    explicit SPIRVInstructionIterator(std::span<u32> module);

    // Query methods
    spv::Op GetOpcode() const;
    u16 GetWordCount() const;

    bool HasResult() const;
    bool HasResultType() const;

    u32 GetResult() const;
    u32 GetResultType() const;

    // Set result IDs
    void SetResult(u32 id);
    void SetResultType(u32 id);

    // Operand access
    size_t GetOperandCount() const;
    u32 GetOperand(size_t index) const;
    void SetOperand(size_t index, u32 value);

    // Get pointer to operands (for direct manipulation)
    u32* GetOperandPtr(size_t index);
    const u32* GetOperandPtr(size_t index) const;

    // Get span of current instruction
    std::span<u32> GetInstructionSpan();
    std::span<const u32> GetInstructionSpan() const;

    // Raw instruction access
    u32* Data();
    const u32* Data() const;

    // Iterator operations
    SPIRVInstructionIterator& operator++();
    SPIRVInstructionIterator operator++(int);

    SPIRVInstructionIterator& operator--();
    SPIRVInstructionIterator operator--(int);

    bool operator==(const SPIRVInstructionIterator& other) const;
    bool operator!=(const SPIRVInstructionIterator& other) const;

    u32& operator*();
    const u32& operator*() const;

    bool IsValid() const;
    bool IsEnd() const;

  private:
    int GetResultTypeIndex() const;
    int GetResultIndex() const;

    size_t GetOperandStartIndex() const;
  };

public:
  ~SPIRVModule();

  ALWAYS_INLINE SPIRVInstructionIterator begin() { return SPIRVInstructionIterator(m_module); }
  ALWAYS_INLINE SPIRVInstructionIterator end() { return SPIRVInstructionIterator(m_module, m_module.size()); }

  // Header access methods
  ALWAYS_INLINE u32 GetMagicNumber() const { return m_module[0]; }
  ALWAYS_INLINE u32 GetVersion() const { return m_module[1]; }
  ALWAYS_INLINE u32 GetGeneratorMagic() const { return m_module[2]; }
  ALWAYS_INLINE u32 GetBound() const { return m_module[3]; }
  ALWAYS_INLINE u32 GetSchema() const { return m_module[4]; }

  ALWAYS_INLINE std::span<u32> GetData() const { return m_module; }

  void SetBound(u32 bound);

  bool SetDecoration(u32 id, u32 decoration, u32 value, Error* error);

  static std::optional<SPIRVModule> Get(std::span<u32> module, Error* error);

private:
  explicit SPIRVModule(std::span<u32> module);

  static bool ValidateHeader(std::span<const u32> module, Error* error);

  std::span<u32> m_module;
};
