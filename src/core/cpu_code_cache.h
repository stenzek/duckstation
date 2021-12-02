#pragma once
#include "bus.h"
#include "common/bitfield.h"
#include "common/jit_code_buffer.h"
#include "common/page_fault_handler.h"
#include "cpu_types.h"
#include <array>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#ifdef WITH_RECOMPILER
#include "cpu_recompiler_types.h"
#endif

namespace CPU {

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
  bool is_direct_branch_instruction : 1;
  bool is_unconditional_branch_instruction : 1;
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
  using HostCodePointer = void (*)();

  struct LinkInfo
  {
    CodeBlock* block;
    void* host_pc;
    void* host_resolve_pc;
    u32 host_pc_size;
  };

  CodeBlock(const CodeBlockKey key_) : key(key_) {}

  CodeBlockKey key;
  u32 host_code_size = 0;
  HostCodePointer host_code = nullptr;

  std::vector<CodeBlockInstruction> instructions;
  std::vector<LinkInfo> link_predecessors;
  std::vector<LinkInfo> link_successors;

  TickCount uncached_fetch_ticks = 0;
  u32 icache_line_count = 0;

#ifdef WITH_RECOMPILER
  std::vector<Recompiler::LoadStoreBackpatchInfo> loadstore_backpatch_info;
#endif

  bool contains_loadstore_instructions = false;
  bool contains_double_branches = false;
  bool invalidated = false;
  bool can_link = true;

  u32 recompile_frame_number = 0;
  u32 recompile_count = 0;
  u32 invalidate_frame_number = 0;

  const u32 GetPC() const { return key.GetPC(); }
  const u32 GetSizeInBytes() const { return static_cast<u32>(instructions.size()) * sizeof(Instruction); }
  const u32 GetStartPageIndex() const { return (key.GetPCPhysicalAddress() / HOST_PAGE_SIZE); }
  const u32 GetEndPageIndex() const
  {
    return ((key.GetPCPhysicalAddress() + GetSizeInBytes()) / HOST_PAGE_SIZE);
  }
  bool IsInRAM() const
  {
    // TODO: Constant
    return key.GetPCPhysicalAddress() < 0x200000;
  }
};

namespace CodeCache {

enum : u32
{
  FAST_MAP_TABLE_COUNT = 0x10000,
  FAST_MAP_TABLE_SIZE = 0x10000 / 4, // 16384
  FAST_MAP_TABLE_SHIFT = 16,
};

using FastMapTable = CodeBlock::HostCodePointer*;

void Initialize();
void Shutdown();
void Execute();

#ifdef WITH_RECOMPILER
using DispatcherFunction = void (*)();
using SingleBlockDispatcherFunction = void(*)(const CodeBlock::HostCodePointer);

FastMapTable* GetFastMapPointer();
void ExecuteRecompiler();
#endif

/// Flushes the code cache, forcing all blocks to be recompiled.
void Flush();

/// Changes whether the recompiler is enabled.
void Reinitialize();

/// Invalidates all blocks which are in the range of the specified code page.
void InvalidateBlocksWithPageIndex(u32 page_index);

/// Invalidates all blocks in the cache.
void InvalidateAll();

template<PGXPMode pgxp_mode>
void InterpretCachedBlock(const CodeBlock& block);

template<PGXPMode pgxp_mode>
void InterpretUncachedBlock();

/// Invalidates any code pages which overlap the specified range.
ALWAYS_INLINE void InvalidateCodePages(PhysicalMemoryAddress address, u32 word_count)
{
  const u32 start_page = address / HOST_PAGE_SIZE;
  const u32 end_page = (address + word_count * sizeof(u32) - sizeof(u32)) / HOST_PAGE_SIZE;
  for (u32 page = start_page; page <= end_page; page++)
  {
    if (Bus::m_ram_code_bits[page])
      CPU::CodeCache::InvalidateBlocksWithPageIndex(page);
  }
}

}; // namespace CodeCache

} // namespace CPU
