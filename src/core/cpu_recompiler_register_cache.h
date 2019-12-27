#pragma once
#include "YBaseLib/Assert.h"
#include "cpu_recompiler_types.h"
#include "cpu_types.h"

#include <array>
#include <optional>
#include <stack>
#include <tuple>

namespace CPU::Recompiler {

enum class HostRegState : u8
{
  None = 0,
  Usable = (1 << 1),               // Can be allocated
  CallerSaved = (1 << 2),          // Register is caller-saved, and should be saved/restored after calling a function.
  CalleeSaved = (1 << 3),          // Register is callee-saved, and should be restored after leaving the block.
  InUse = (1 << 4),                // In-use, must be saved/restored across function call.
  CalleeSavedAllocated = (1 << 5), // Register was callee-saved and allocated, so should be restored before returning.
  Discarded = (1 << 6),            // Register contents is not used, so do not preserve across function calls.
};
IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(HostRegState);

enum class ValueFlags : u8
{
  None = 0,
  Valid = (1 << 0),
  Constant = (1 << 1),       // The value itself is constant, and not in a register.
  InHostRegister = (1 << 2), // The value itself is located in a host register.
  Scratch = (1 << 3),        // The value is temporary, and will be released after the Value is destroyed.
  Dirty = (1 << 4),          // For register cache values, the value needs to be written back to the CPU struct.
};
IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(ValueFlags);

struct Value
{
  RegisterCache* regcache = nullptr;
  u64 constant_value = 0;
  HostReg host_reg = {};

  RegSize size = RegSize_8;
  ValueFlags flags = ValueFlags::None;

  Value();
  Value(RegisterCache* regcache_, u64 constant_, RegSize size_, ValueFlags flags_);
  Value(RegisterCache* regcache_, HostReg reg_, RegSize size_, ValueFlags flags_);
  Value(const Value& other);
  Value(Value&& other);
  ~Value();

  Value& operator=(const Value& other);
  Value& operator=(Value&& other);

  bool IsConstant() const { return (flags & ValueFlags::Constant) != ValueFlags::None; }
  bool IsValid() const { return (flags & ValueFlags::Valid) != ValueFlags::None; }
  bool IsInHostRegister() const { return (flags & ValueFlags::InHostRegister) != ValueFlags::None; }
  bool IsScratch() const { return (flags & ValueFlags::Scratch) != ValueFlags::None; }

  /// Returns the host register this value is bound to.
  HostReg GetHostRegister() const
  {
    DebugAssert(IsInHostRegister());
    return host_reg;
  }

  /// Returns true if this value is constant and has the specified value.
  bool HasConstantValue(u64 cv) const
  {
    return (((flags & ValueFlags::Constant) != ValueFlags::None) && constant_value == cv);
  }

  /// Removes the contents of this value. Use with care, as scratch/temporaries are not released.
  void Clear();

  /// Releases the host register if needed, and clears the contents.
  void ReleaseAndClear();

  /// Flags the value is being discarded. Call Undiscard() to track again.
  void Discard();
  void Undiscard();

  void AddHostReg(RegisterCache* regcache_, HostReg hr)
  {
    DebugAssert(IsValid());
    regcache = regcache_;
    host_reg = hr;
    flags |= ValueFlags::InHostRegister;
  }

  void SetHostReg(RegisterCache* regcache_, HostReg hr, RegSize size_)
  {
    regcache = regcache_;
    constant_value = 0;
    host_reg = hr;
    size = size_;
    flags = ValueFlags::Valid | ValueFlags::InHostRegister;
  }

  void ClearConstant()
  {
    // By clearing the constant bit, we should already be in a host register.
    DebugAssert(IsInHostRegister());
    flags &= ~ValueFlags::Constant;
  }

  bool IsDirty() const { return (flags & ValueFlags::Dirty) != ValueFlags::None; }
  void SetDirty() { flags |= ValueFlags::Dirty; }
  void ClearDirty() { flags &= ~ValueFlags::Dirty; }

  /// Returns the same register viewed as a different size.
  Value ViewAsSize(RegSize view_size) const
  {
    if (view_size == size)
      return *this;

    if (IsConstant())
    {
      // truncate to size
      switch (view_size)
      {
        case RegSize_8:
          return Value::FromConstant(constant_value & UINT64_C(0xFF), RegSize_8);
        case RegSize_16:
          return Value::FromConstant(constant_value & UINT64_C(0xFFFF), RegSize_16);
        case RegSize_32:
          return Value::FromConstant(constant_value & UINT64_C(0xFFFFFFFF), RegSize_32);
        case RegSize_64:
        default:
          return Value::FromConstant(constant_value, view_size);
      }
    }

    if (IsInHostRegister())
      return Value::FromHostReg(regcache, host_reg, view_size);

    // invalid?
    return Value();
  }

  /// Returns the constant value as a signed 64-bit integer, suitable as an immediate.
  s64 GetS64ConstantValue() const
  {
    switch (size)
    {
      case RegSize_8:
        return static_cast<s64>(SignExtend64(Truncate8(constant_value)));

      case RegSize_16:
        return static_cast<s64>(SignExtend64(Truncate16(constant_value)));

      case RegSize_32:
        return static_cast<s64>(SignExtend64(Truncate32(constant_value)));

      case RegSize_64:
      default:
        return static_cast<s64>(constant_value);
    }
  }

  static Value FromHostReg(RegisterCache* regcache, HostReg reg, RegSize size)
  {
    return Value(regcache, reg, size, ValueFlags::Valid | ValueFlags::InHostRegister);
  }
  static Value FromScratch(RegisterCache* regcache, HostReg reg, RegSize size)
  {
    return Value(regcache, reg, size, ValueFlags::Valid | ValueFlags::InHostRegister | ValueFlags::Scratch);
  }
  static Value FromConstant(u64 cv, RegSize size)
  {
    return Value(nullptr, cv, size, ValueFlags::Valid | ValueFlags::Constant);
  }
  static Value FromConstantU8(u8 value) { return FromConstant(ZeroExtend64(value), RegSize_8); }
  static Value FromConstantU16(u16 value) { return FromConstant(ZeroExtend64(value), RegSize_16); }
  static Value FromConstantU32(u32 value) { return FromConstant(ZeroExtend64(value), RegSize_32); }
  static Value FromConstantU64(u64 value) { return FromConstant(value, RegSize_64); }

private:
  void Release();
};

class RegisterCache
{
public:
  RegisterCache(CodeGenerator& code_generator);
  ~RegisterCache();

  u32 GetActiveCalleeSavedRegisterCount() const { return m_state.callee_saved_order_count; }

  //////////////////////////////////////////////////////////////////////////
  // Register Allocation
  //////////////////////////////////////////////////////////////////////////
  void SetHostRegAllocationOrder(std::initializer_list<HostReg> regs);
  void SetCallerSavedHostRegs(std::initializer_list<HostReg> regs);
  void SetCalleeSavedHostRegs(std::initializer_list<HostReg> regs);
  void SetCPUPtrHostReg(HostReg reg);

  /// Returns true if the register is permitted to be used in the register cache.
  bool IsUsableHostReg(HostReg reg) const;
  bool IsHostRegInUse(HostReg reg) const;
  bool HasFreeHostRegister() const;
  u32 GetUsedHostRegisters() const;
  u32 GetFreeHostRegisters() const;

  /// Allocates a new host register. If there are no free registers, the guest register which was accessed the longest
  /// time ago will be evicted.
  HostReg AllocateHostReg(HostRegState state = HostRegState::InUse);

  /// Allocates a specific host register. If this register is not free, returns false.
  bool AllocateHostReg(HostReg reg, HostRegState state = HostRegState::InUse);

  /// Flags the host register as discard-able. This means that the contents is no longer required, and will not be
  /// pushed when saving caller-saved registers.
  void DiscardHostReg(HostReg reg);

  /// Clears the discard-able flag on a host register, so that the contents will be preserved across function calls.
  void UndiscardHostReg(HostReg reg);

  /// Frees a host register, making it usable in future allocations.
  void FreeHostReg(HostReg reg);

  /// Ensures a host register is free, removing any value cached.
  void EnsureHostRegFree(HostReg reg);

  /// Push/pop volatile host registers. Returns the number of registers pushed/popped.
  u32 PushCallerSavedRegisters() const;
  u32 PopCallerSavedRegisters() const;

  /// Restore callee-saved registers. Call at the end of the function.
  u32 PopCalleeSavedRegisters(bool commit);

  /// Pushes the register allocator state, use when entering branched code.
  void PushState();

  /// Pops the register allocator state, use when leaving branched code.
  void PopState();

  //////////////////////////////////////////////////////////////////////////
  // Scratch Register Allocation
  //////////////////////////////////////////////////////////////////////////
  Value GetCPUPtr();
  Value AllocateScratch(RegSize size, HostReg reg = HostReg_Invalid);

  //////////////////////////////////////////////////////////////////////////
  // Guest Register Caching
  //////////////////////////////////////////////////////////////////////////

  /// Returns true if the specified guest register is cached.
  bool IsGuestRegisterCached(Reg guest_reg) const
  {
    const Value& cache_value = m_state.guest_reg_state[static_cast<u8>(guest_reg)];
    return cache_value.IsConstant() || cache_value.IsInHostRegister();
  }

  /// Returns true if the specified guest register is cached and in a host register.
  bool IsGuestRegisterInHostRegister(Reg guest_reg) const
  {
    const Value& cache_value = m_state.guest_reg_state[static_cast<u8>(guest_reg)];
    return cache_value.IsInHostRegister();
  }

  /// Returns the host register if the guest register is cached.
  std::optional<HostReg> GetHostRegisterForGuestRegister(Reg guest_reg) const
  {
    if (!m_state.guest_reg_state[static_cast<u8>(guest_reg)].IsInHostRegister())
      return std::nullopt;
    return m_state.guest_reg_state[static_cast<u8>(guest_reg)].GetHostRegister();
  }

  /// Returns true if there is a load delay which will be stored at the end of the instruction.
  bool HasLoadDelay() const { return m_state.load_delay_register != Reg::count; }

  Value ReadGuestRegister(Reg guest_reg, bool cache = true, bool force_host_register = false,
                          HostReg forced_host_reg = HostReg_Invalid);

  /// Reads the guest register to a caller-owned scratch register. This will ensure the cache won't invalidate the value
  /// from some other write.
  Value ReadGuestRegisterToScratch(Reg guest_reg);

  /// Creates a copy of value, and stores it to guest_reg.
  Value WriteGuestRegister(Reg guest_reg, Value&& value);

  /// Stores the specified value to the guest register after the next instruction (load delay).
  void WriteGuestRegisterDelayed(Reg guest_reg, Value&& value);

  /// Moves load delay to the next load delay, and writes any previous load delay to the destination register.
  void UpdateLoadDelay();

  /// Writes the load delay to the CPU structure, so it is synced up with the interpreter.
  void WriteLoadDelayToCPU(bool clear);

  /// Flushes the load delay, i.e. writes it to the destination register.
  void FlushLoadDelay(bool clear);

  void FlushGuestRegister(Reg guest_reg, bool invalidate, bool clear_dirty);
  void InvalidateGuestRegister(Reg guest_reg);

  void InvalidateAllNonDirtyGuestRegisters();
  void FlushAllGuestRegisters(bool invalidate, bool clear_dirty);
  bool EvictOneGuestRegister();

private:
  void ClearRegisterFromOrder(Reg reg);
  void PushRegisterToOrder(Reg reg);
  void AppendRegisterToOrder(Reg reg);

  CodeGenerator& m_code_generator;

  std::array<HostReg, HostReg_Count> m_host_register_allocation_order{};

  HostReg m_cpu_ptr_host_register = {};

  struct RegAllocState
  {
    std::array<HostRegState, HostReg_Count> host_reg_state{};
    std::array<HostReg, HostReg_Count> callee_saved_order{};
    std::array<Value, static_cast<u8>(Reg::count)> guest_reg_state{};
    std::array<Reg, HostReg_Count> guest_reg_order{};

    u32 available_count = 0;
    u32 callee_saved_order_count = 0;
    u32 guest_reg_order_count = 0;

    Reg load_delay_register = Reg::count;
    Value load_delay_value{};

    Reg next_load_delay_register = Reg::count;
    Value next_load_delay_value{};
  } m_state;

  std::stack<RegAllocState> m_state_stack;
};

} // namespace CPU::Recompiler