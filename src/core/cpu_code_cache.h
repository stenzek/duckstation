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

  CodeBlockKey GetNextBlockKey() const;
  CodeBlock* LookupBlock(CodeBlockKey key);
  bool RevalidateBlock(CodeBlock* block);
  bool CompileBlock(CodeBlock* block);
  void FlushBlock(CodeBlock* block);
  void AddBlockToPageMap(CodeBlock* block);
  void RemoveBlockFromPageMap(CodeBlock* block);
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