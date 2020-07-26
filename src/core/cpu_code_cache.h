#pragma once
#include "common/bitfield.h"
#include "common/jit_code_buffer.h"
#include "cpu_types.h"
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

class Bus;

namespace CPU {
class Core;

union CodeBlockKey
{
  u32 bits;

  BitField<u32, bool, 0, 1> user_mode;
  BitField<u32, u32, 2, 30> aligned_pc;

  ALWAYS_INLINE u32 GetPC() const { return aligned_pc << 2; }
  ALWAYS_INLINE void SetPC(u32 pc) { aligned_pc = pc >> 2; }

  ALWAYS_INLINE u32 GetPCPhysicalAddress() const { return (aligned_pc << 2) & PHYSICAL_MEMORY_ADDRESS_MASK; }

  ALWAYS_INLINE CodeBlockKey& operator=(const CodeBlockKey& rhs)
  {
    bits = rhs.bits;
    return *this;
  }

  ALWAYS_INLINE bool operator==(const CodeBlockKey& rhs) const { return bits == rhs.bits; }
  ALWAYS_INLINE bool operator!=(const CodeBlockKey& rhs) const { return bits != rhs.bits; }
  ALWAYS_INLINE bool operator<(const CodeBlockKey& rhs) const { return bits < rhs.bits; }
};

struct CodeBlockInstruction
{
  Instruction instruction;
  u32 pc;

  bool is_branch_instruction : 1;
  bool is_branch_delay_slot : 1;
  bool is_load_instruction : 1;
  bool is_store_instruction : 1;
  bool is_load_delay_slot : 1;
  bool is_last_instruction : 1;
  bool has_load_delay : 1;
  bool can_trap : 1;
};

struct CodeBlock
{
  using HostCodePointer = void (*)(Core*);

  CodeBlock(const CodeBlockKey key_) : key(key_) {}

  CodeBlockKey key;
  u32 host_code_size = 0;
  HostCodePointer host_code = nullptr;

  std::vector<CodeBlockInstruction> instructions;
  std::vector<CodeBlock*> link_predecessors;
  std::vector<CodeBlock*> link_successors;

  bool invalidated = false;

  const u32 GetPC() const { return key.GetPC(); }
  const u32 GetSizeInBytes() const { return static_cast<u32>(instructions.size()) * sizeof(Instruction); }
  const u32 GetStartPageIndex() const { return (key.GetPCPhysicalAddress() / CPU_CODE_CACHE_PAGE_SIZE); }
  const u32 GetEndPageIndex() const
  {
    return ((key.GetPCPhysicalAddress() + GetSizeInBytes()) / CPU_CODE_CACHE_PAGE_SIZE);
  }
  bool IsInRAM() const
  {
    // TODO: Constant
    return key.GetPCPhysicalAddress() < 0x200000;
  }
};

namespace CodeCache {

void Initialize(Core* core, bool use_recompiler);
void Shutdown();
void Execute();

/// Flushes the code cache, forcing all blocks to be recompiled.
void Flush();

/// Changes whether the recompiler is enabled.
void SetUseRecompiler(bool enable);

/// Invalidates all blocks which are in the range of the specified code page.
void InvalidateBlocksWithPageIndex(u32 page_index);

}; // namespace CodeCache

} // namespace CPU
