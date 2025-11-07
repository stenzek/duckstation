// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#define SPV_ENABLE_UTILITY_CODE

#include "spirv_module.h"

#include "common/assert.h"
#include "common/error.h"

SPIRVModule::SPIRVModule(std::span<u32> module) : m_module(module)
{
  DebugAssert(ValidateHeader(module, nullptr));
}

SPIRVModule::~SPIRVModule() = default;

void SPIRVModule::SetBound(u32 bound)
{
  m_module[3] = bound;
}

bool SPIRVModule::SetDecoration(u32 id, u32 decoration, u32 value, Error* error)
{
  for (auto it = begin(); it != end(); ++it)
  {
    if (it.GetOpcode() == spv::Op::OpDecorate)
    {
      const u32 target_id = it.GetOperand(0);
      const u32 decor = it.GetOperand(1);
      if (target_id == id && decor == decoration)
      {
        // Found existing decoration, update value
        if (it.GetOperandCount() < 3)
        {
          Error::SetStringView(error, "Existing decoration has no value to update");
          return false;
        }
        it.SetOperand(2, value);
        return true;
      }
    }
  }
#if 0
  // Decoration not found, append new one
  size_t old_size = m_module.size();
  m_module = m_module.subspan(0, old_size + 3); // OpDecorate with 3 operands
  m_module[old_size + 0] = (3 << 16) | static_cast<u32>(spv::Op::OpDecorate);
  m_module[old_size + 1] = id;
  m_module[old_size + 2] = decoration;
  m_module[old_size + 3] = value;
#endif

  Error::SetStringFmt(error, "OpDecorate({}) not found for {}", decoration, id);
  return false;
}

std::optional<SPIRVModule> SPIRVModule::Get(std::span<u32> module, Error* error)
{
  if (!ValidateHeader(module, error))
    return std::nullopt;

  return SPIRVModule(module);
}

bool SPIRVModule::ValidateHeader(std::span<const u32> module, Error* error)
{
  if (module.size() < HEADER_SIZE)
  {
    Error::SetStringView(error, "Invalid SPIR-V module: too small for header");
    return false;
  }

  if (module[0] != spv::MagicNumber)
  {
    Error::SetStringView(error, "Invalid SPIR-V magic number");
    return false;
  }

  return true;
}

bool SPIRVModule::SPIRVInstructionIterator::operator!=(const SPIRVInstructionIterator& other) const
{
  return !(*this == other);
}

bool SPIRVModule::SPIRVInstructionIterator::operator==(const SPIRVInstructionIterator& other) const
{
  return m_module.data() == other.m_module.data() && m_offset == other.m_offset;
}

SPIRVModule::SPIRVInstructionIterator::SPIRVInstructionIterator(std::span<u32> module)
  // Skip SPIR-V header (5 words)
  : m_module(module), m_offset(HEADER_SIZE)
{
  DebugAssert(m_module.size() >= HEADER_SIZE);
}

SPIRVModule::SPIRVInstructionIterator::SPIRVInstructionIterator(std::span<u32> module, size_t offset)
  : m_module(module), m_offset(offset)
{
}

SPIRVModule::SPIRVInstructionIterator::SPIRVInstructionIterator() : m_module(), m_offset(0)
{
}

spv::Op SPIRVModule::SPIRVInstructionIterator::GetOpcode() const
{
  DebugAssert(IsValid());
  return static_cast<spv::Op>(m_module[m_offset] & 0xFFFF);
}

u16 SPIRVModule::SPIRVInstructionIterator::GetWordCount() const
{
  DebugAssert(IsValid());
  return static_cast<u16>(std::min<size_t>(m_module[m_offset] >> 16, m_module.size() - m_offset));
}

bool SPIRVModule::SPIRVInstructionIterator::HasResult() const
{
  return GetResultIndex() != -1;
}

bool SPIRVModule::SPIRVInstructionIterator::HasResultType() const
{
  return GetResultTypeIndex() != -1;
}

u32 SPIRVModule::SPIRVInstructionIterator::GetResult() const
{
  const int idx = GetResultIndex();
  if (idx == -1)
    Panic("Instruction has no result ID");

  return m_module[m_offset + idx];
}

u32 SPIRVModule::SPIRVInstructionIterator::GetResultType() const
{
  const int idx = GetResultTypeIndex();
  if (idx == -1)
    Panic("Instruction has no result type ID");

  return m_module[m_offset + idx];
}

void SPIRVModule::SPIRVInstructionIterator::SetResult(u32 id)
{
  const int idx = GetResultIndex();
  if (idx == -1)
    Panic("Instruction has no result ID");

  m_module[m_offset + idx] = id;
}

void SPIRVModule::SPIRVInstructionIterator::SetResultType(u32 id)
{
  const int idx = GetResultTypeIndex();
  if (idx == -1)
    Panic("Instruction has no result type ID");

  m_module[m_offset + idx] = id;
}

size_t SPIRVModule::SPIRVInstructionIterator::GetOperandCount() const
{
  DebugAssert(IsValid());

  size_t count = GetWordCount() - 1; // Subtract opcode word
  if (HasResultType())
    count--;
  if (HasResult())
    count--;
  return count;
}

u32 SPIRVModule::SPIRVInstructionIterator::GetOperand(size_t index) const
{
  const size_t actual_index = GetOperandStartIndex() + index;
  if (actual_index >= GetWordCount())
    Panic("Operand index out of range");

  return m_module[m_offset + actual_index];
}

void SPIRVModule::SPIRVInstructionIterator::SetOperand(size_t index, u32 value)
{
  const size_t actual_index = GetOperandStartIndex() + index;
  if (actual_index >= GetWordCount())
    Panic("Operand index out of range");

  m_module[m_offset + actual_index] = value;
}

const u32* SPIRVModule::SPIRVInstructionIterator::GetOperandPtr(size_t index) const
{
  const size_t actual_index = GetOperandStartIndex() + index;
  if (actual_index >= GetWordCount())
    Panic("Operand index out of range");

  return &m_module[m_offset + actual_index];
}

u32* SPIRVModule::SPIRVInstructionIterator::GetOperandPtr(size_t index)
{
  const size_t actual_index = GetOperandStartIndex() + index;
  if (actual_index >= GetWordCount())
    Panic("Operand index out of range");

  return &m_module[m_offset + actual_index];
}

std::span<const u32> SPIRVModule::SPIRVInstructionIterator::GetInstructionSpan() const
{
  DebugAssert(IsValid());
  return m_module.subspan(m_offset, GetWordCount());
}

std::span<u32> SPIRVModule::SPIRVInstructionIterator::GetInstructionSpan()
{
  DebugAssert(IsValid());
  return m_module.subspan(m_offset, GetWordCount());
}

const u32* SPIRVModule::SPIRVInstructionIterator::Data() const
{
  DebugAssert(IsValid());
  return &m_module[m_offset];
}

u32* SPIRVModule::SPIRVInstructionIterator::Data()
{
  DebugAssert(IsValid());
  return &m_module[m_offset];
}

SPIRVModule::SPIRVInstructionIterator SPIRVModule::SPIRVInstructionIterator::operator++(int)
{
  SPIRVInstructionIterator tmp = *this;
  ++(*this);
  return tmp;
}

SPIRVModule::SPIRVModule::SPIRVInstructionIterator& SPIRVModule::SPIRVInstructionIterator::operator++()
{
  if (m_offset >= m_module.size())
    Panic("Cannot increment past end");

  m_offset += GetWordCount();
  return *this;
}

SPIRVModule::SPIRVInstructionIterator SPIRVModule::SPIRVInstructionIterator::operator--(int)
{
  SPIRVInstructionIterator tmp = *this;
  --(*this);
  return tmp;
}

SPIRVModule::SPIRVModule::SPIRVInstructionIterator& SPIRVModule::SPIRVInstructionIterator::operator--()
{
  if (m_offset <= HEADER_SIZE)
    Panic("Cannot decrement past beginning");

  // Search backwards for instruction start
  size_t prev = m_offset - 1;
  while (prev >= HEADER_SIZE)
  {
    u16 wordCount = static_cast<u16>(m_module[prev] >> 16);
    if (wordCount > 0 && prev + wordCount == m_offset)
    {
      m_offset = prev;
      return *this;
    }
    if (prev == 0)
      break;
    --prev;
  }
  Panic("Failed to find previous instruction");
}

const u32& SPIRVModule::SPIRVInstructionIterator::operator*() const
{
  return m_module[m_offset];
}

u32& SPIRVModule::SPIRVInstructionIterator::operator*()
{
  return m_module[m_offset];
}

bool SPIRVModule::SPIRVInstructionIterator::IsValid() const
{
  return m_offset < m_module.size();
}

bool SPIRVModule::SPIRVInstructionIterator::IsEnd() const
{
  return m_offset >= m_module.size();
}

int SPIRVModule::SPIRVInstructionIterator::GetResultTypeIndex() const
{
  const spv::Op op = GetOpcode();
  bool has_result, has_result_type;
  spv::HasResultAndType(op, &has_result, &has_result_type);
  return (has_result && has_result_type) ? 1 : -1;
}

int SPIRVModule::SPIRVInstructionIterator::GetResultIndex() const
{
  spv::Op op = GetOpcode();
  bool has_result, has_result_type;
  spv::HasResultAndType(op, &has_result, &has_result_type);
  if (has_result && has_result_type)
    return 2;
  else if (op == spv::Op::OpLabel || op == spv::Op::OpString || op == spv::Op::OpExtInstImport)
    return 1;

  return -1;
}

size_t SPIRVModule::SPIRVInstructionIterator::GetOperandStartIndex() const
{
  size_t idx = 1; // Skip opcode word
  if (HasResultType())
    idx++;
  if (HasResult())
    idx++;
  return idx;
}
