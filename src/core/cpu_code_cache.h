#pragma once
#include "common/bitfield.h"
#include "cpu_types.h"
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

class JitCodeBuffer;

class Bus;
class System;

namespace CPU {
class Core;

namespace Recompiler {
class ASMFunctions;
}

class CodeCache
{
public:
  CodeCache();
  ~CodeCache();

  void Initialize(System* system, Core* core, Bus* bus);
  void Reset();
  void Execute();

  /// Invalidates all blocks which are in the range of the specified code page.
  void InvalidateBlocksWithPageIndex(u32 page_index);

private:
  using BlockMap = std::unordered_map<u32, CodeBlock*>;

  void LogCurrentState();

  /// Returns the block key for the current execution state.
  CodeBlockKey GetNextBlockKey() const;

  /// Looks up the block in the cache if it's already been compiled.
  CodeBlock* LookupBlock(CodeBlockKey key);

  /// Can the current block execute? This will re-validate the block if necessary.
  /// The block can also be flushed if recompilation failed, so ignore the pointer if false is returned.
  bool RevalidateBlock(CodeBlock* block);

  bool CompileBlock(CodeBlock* block);
  void FlushBlock(CodeBlock* block);
  void AddBlockToPageMap(CodeBlock* block);
  void RemoveBlockFromPageMap(CodeBlock* block);

  /// Link block from to to.
  void LinkBlock(CodeBlock* from, CodeBlock* to);

  /// Unlink all blocks which point to this block, and any that this block links to.
  void UnlinkBlock(CodeBlock* block);

  void InterpretCachedBlock(const CodeBlock& block);
  void InterpretUncachedBlock();

  System* m_system;
  Core* m_core;
  Bus* m_bus;

  std::unique_ptr<JitCodeBuffer> m_code_buffer;
  std::unique_ptr<Recompiler::ASMFunctions> m_asm_functions;

  BlockMap m_blocks;

  std::array<std::vector<CodeBlock*>, CPU_CODE_CACHE_PAGE_COUNT> m_ram_block_map;
};

extern bool USE_CODE_CACHE;
extern bool USE_RECOMPILER;

} // namespace CPU