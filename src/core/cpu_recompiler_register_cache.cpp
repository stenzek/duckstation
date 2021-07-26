#include "cpu_recompiler_register_cache.h"
#include "common/log.h"
#include "cpu_recompiler_code_generator.h"
#include <cinttypes>
Log_SetChannel(CPU::Recompiler);

namespace CPU::Recompiler {

Value::Value() = default;

Value::Value(RegisterCache* regcache_, u64 constant_, RegSize size_, ValueFlags flags_)
  : regcache(regcache_), constant_value(constant_), size(size_), flags(flags_)
{
}

Value::Value(const Value& other)
  : regcache(other.regcache), constant_value(other.constant_value), host_reg(other.host_reg), size(other.size),
    flags(other.flags)
{
  AssertMsg(!other.IsScratch(), "Can't copy a temporary register");
}

Value::Value(Value&& other)
  : regcache(other.regcache), constant_value(other.constant_value), host_reg(other.host_reg), size(other.size),
    flags(other.flags)
{
  other.Clear();
}

Value::Value(RegisterCache* regcache_, HostReg reg_, RegSize size_, ValueFlags flags_)
  : regcache(regcache_), host_reg(reg_), size(size_), flags(flags_)
{
}

Value::~Value()
{
  Release();
}

Value& Value::operator=(const Value& other)
{
  AssertMsg(!other.IsScratch(), "Can't copy a temporary register");

  Release();
  regcache = other.regcache;
  constant_value = other.constant_value;
  host_reg = other.host_reg;
  size = other.size;
  flags = other.flags;

  return *this;
}

Value& Value::operator=(Value&& other)
{
  Release();
  regcache = other.regcache;
  constant_value = other.constant_value;
  host_reg = other.host_reg;
  size = other.size;
  flags = other.flags;
  other.Clear();
  return *this;
}

void Value::Clear()
{
  regcache = nullptr;
  constant_value = 0;
  host_reg = {};
  size = RegSize_8;
  flags = ValueFlags::None;
}

void Value::Release()
{
  if (IsScratch())
  {
    DebugAssert(IsInHostRegister() && regcache);
    regcache->FreeHostReg(host_reg);
  }
}

void Value::ReleaseAndClear()
{
  Release();
  Clear();
}

void Value::Discard()
{
  DebugAssert(IsInHostRegister());
  regcache->DiscardHostReg(host_reg);
}

void Value::Undiscard()
{
  DebugAssert(IsInHostRegister());
  regcache->UndiscardHostReg(host_reg);
}

RegisterCache::RegisterCache(CodeGenerator& code_generator) : m_code_generator(code_generator)
{
  m_state.guest_reg_order.fill(Reg::count);
}

RegisterCache::~RegisterCache()
{
  Assert(m_state_stack.empty());
}

void RegisterCache::SetHostRegAllocationOrder(std::initializer_list<HostReg> regs)
{
  size_t index = 0;
  for (HostReg reg : regs)
  {
    m_state.host_reg_state[reg] = HostRegState::Usable;
    m_host_register_allocation_order[index++] = reg;
  }
  m_state.available_count = static_cast<u32>(index);
}

void RegisterCache::SetCallerSavedHostRegs(std::initializer_list<HostReg> regs)
{
  for (HostReg reg : regs)
    m_state.host_reg_state[reg] |= HostRegState::CallerSaved;
}

void RegisterCache::SetCalleeSavedHostRegs(std::initializer_list<HostReg> regs)
{
  for (HostReg reg : regs)
    m_state.host_reg_state[reg] |= HostRegState::CalleeSaved;
}

void RegisterCache::SetCPUPtrHostReg(HostReg reg)
{
  m_cpu_ptr_host_register = reg;
}

bool RegisterCache::IsUsableHostReg(HostReg reg) const
{
  return (m_state.host_reg_state[reg] & HostRegState::Usable) != HostRegState::None;
}

bool RegisterCache::IsHostRegInUse(HostReg reg) const
{
  return (m_state.host_reg_state[reg] & HostRegState::InUse) != HostRegState::None;
}

bool RegisterCache::HasFreeHostRegister() const
{
  for (const HostRegState state : m_state.host_reg_state)
  {
    if ((state & (HostRegState::Usable | HostRegState::InUse)) == (HostRegState::Usable))
      return true;
  }

  return false;
}

u32 RegisterCache::GetUsedHostRegisters() const
{
  u32 count = 0;
  for (const HostRegState state : m_state.host_reg_state)
  {
    if ((state & (HostRegState::Usable | HostRegState::InUse)) == (HostRegState::Usable | HostRegState::InUse))
      count++;
  }

  return count;
}

u32 RegisterCache::GetFreeHostRegisters() const
{
  u32 count = 0;
  for (const HostRegState state : m_state.host_reg_state)
  {
    if ((state & (HostRegState::Usable | HostRegState::InUse)) == (HostRegState::Usable))
      count++;
  }

  return count;
}

HostReg RegisterCache::AllocateHostReg(HostRegState state /* = HostRegState::InUse */)
{
  if (m_state.allocator_inhibit_count > 0)
    Panic("Allocating when inhibited");

  // try for a free register in allocation order
  for (u32 i = 0; i < m_state.available_count; i++)
  {
    const HostReg reg = m_host_register_allocation_order[i];
    if ((m_state.host_reg_state[reg] & (HostRegState::Usable | HostRegState::InUse)) == HostRegState::Usable)
    {
      if (AllocateHostReg(reg, state))
        return reg;
    }
  }

  // evict one of the cached guest registers
  if (!EvictOneGuestRegister())
    Panic("Failed to evict guest register for new allocation");

  return AllocateHostReg(state);
}

bool RegisterCache::AllocateHostReg(HostReg reg, HostRegState state /*= HostRegState::InUse*/)
{
  if ((m_state.host_reg_state[reg] & HostRegState::InUse) == HostRegState::InUse)
    return false;

  m_state.host_reg_state[reg] |= state;

  if ((m_state.host_reg_state[reg] & (HostRegState::CalleeSaved | HostRegState::CalleeSavedAllocated)) ==
      HostRegState::CalleeSaved)
  {
    // new register we need to save..
    DebugAssert(m_state.callee_saved_order_count < HostReg_Count);
    m_code_generator.EmitPushHostReg(reg, GetActiveCalleeSavedRegisterCount());
    m_state.callee_saved_order[m_state.callee_saved_order_count++] = reg;
    m_state.host_reg_state[reg] |= HostRegState::CalleeSavedAllocated;
  }

  return reg;
}

void RegisterCache::DiscardHostReg(HostReg reg)
{
  DebugAssert(IsHostRegInUse(reg));
  Log_DebugPrintf("Discarding host register %s", m_code_generator.GetHostRegName(reg));
  m_state.host_reg_state[reg] |= HostRegState::Discarded;
}

void RegisterCache::UndiscardHostReg(HostReg reg)
{
  DebugAssert(IsHostRegInUse(reg));
  Log_DebugPrintf("Undiscarding host register %s", m_code_generator.GetHostRegName(reg));
  m_state.host_reg_state[reg] &= ~HostRegState::Discarded;
}

void RegisterCache::FreeHostReg(HostReg reg)
{
  DebugAssert(IsHostRegInUse(reg));
  Log_DebugPrintf("Freeing host register %s", m_code_generator.GetHostRegName(reg));
  m_state.host_reg_state[reg] &= ~HostRegState::InUse;
}

void RegisterCache::EnsureHostRegFree(HostReg reg)
{
  if (!IsHostRegInUse(reg))
    return;

  for (u8 i = 0; i < static_cast<u8>(Reg::count); i++)
  {
    if (m_state.guest_reg_state[i].IsInHostRegister() && m_state.guest_reg_state[i].GetHostRegister() == reg)
      FlushGuestRegister(static_cast<Reg>(i), true, true);
  }
}

Value RegisterCache::GetCPUPtr()
{
  return Value::FromHostReg(this, m_cpu_ptr_host_register, HostPointerSize);
}

Value RegisterCache::AllocateScratch(RegSize size, HostReg reg /* = HostReg_Invalid */)
{
  if (reg == HostReg_Invalid)
  {
    reg = AllocateHostReg();
  }
  else
  {
    Assert(!IsHostRegInUse(reg));
    if (!AllocateHostReg(reg))
      Panic("Failed to allocate specific host register");
  }

  Log_DebugPrintf("Allocating host register %s as scratch", m_code_generator.GetHostRegName(reg));
  return Value::FromScratch(this, reg, size);
}

void RegisterCache::ReserveCallerSavedRegisters()
{
  for (u32 reg = 0; reg < HostReg_Count; reg++)
  {
    if ((m_state.host_reg_state[reg] & (HostRegState::CalleeSaved | HostRegState::CalleeSavedAllocated)) ==
        HostRegState::CalleeSaved)
    {
      DebugAssert(m_state.callee_saved_order_count < HostReg_Count);
      m_code_generator.EmitPushHostReg(static_cast<HostReg>(reg), GetActiveCalleeSavedRegisterCount());
      m_state.callee_saved_order[m_state.callee_saved_order_count++] = static_cast<HostReg>(reg);
      m_state.host_reg_state[reg] |= HostRegState::CalleeSavedAllocated;
    }
  }
}

u32 RegisterCache::PushCallerSavedRegisters() const
{
  u32 position = GetActiveCalleeSavedRegisterCount();
  u32 count = 0;
  for (u32 i = 0; i < HostReg_Count; i++)
  {
    if ((m_state.host_reg_state[i] & (HostRegState::CallerSaved | HostRegState::InUse | HostRegState::Discarded)) ==
        (HostRegState::CallerSaved | HostRegState::InUse))
    {
      m_code_generator.EmitPushHostReg(static_cast<HostReg>(i), position + count);
      count++;
    }
  }

  return count;
}

u32 RegisterCache::PopCallerSavedRegisters() const
{
  u32 count = 0;
  for (u32 i = 0; i < HostReg_Count; i++)
  {
    if ((m_state.host_reg_state[i] & (HostRegState::CallerSaved | HostRegState::InUse | HostRegState::Discarded)) ==
        (HostRegState::CallerSaved | HostRegState::InUse))
    {
      count++;
    }
  }
  if (count == 0)
    return 0;

  u32 position = GetActiveCalleeSavedRegisterCount() + count - 1;
  u32 i = (HostReg_Count - 1);
  do
  {
    if ((m_state.host_reg_state[i] & (HostRegState::CallerSaved | HostRegState::InUse | HostRegState::Discarded)) ==
        (HostRegState::CallerSaved | HostRegState::InUse))
    {
      u32 reg_pair;
      for (reg_pair = (i - 1); reg_pair > 0 && reg_pair < HostReg_Count; reg_pair--)
      {
        if ((m_state.host_reg_state[reg_pair] &
             (HostRegState::CallerSaved | HostRegState::InUse | HostRegState::Discarded)) ==
            (HostRegState::CallerSaved | HostRegState::InUse))
        {
          m_code_generator.EmitPopHostRegPair(static_cast<HostReg>(reg_pair), static_cast<HostReg>(i), position);
          position -= 2;
          i = reg_pair;
          break;
        }
      }

      if (reg_pair == 0)
      {
        m_code_generator.EmitPopHostReg(static_cast<HostReg>(i), position);
        position--;
      }
    }
    i--;
  } while (i > 0);
  return count;
}

u32 RegisterCache::PopCalleeSavedRegisters(bool commit)
{
  if (m_state.callee_saved_order_count == 0)
    return 0;

  u32 count = 0;
  u32 i = m_state.callee_saved_order_count;
  do
  {
    const HostReg reg = m_state.callee_saved_order[i - 1];
    DebugAssert((m_state.host_reg_state[reg] & (HostRegState::CalleeSaved | HostRegState::CalleeSavedAllocated)) ==
                (HostRegState::CalleeSaved | HostRegState::CalleeSavedAllocated));

    if (i > 1)
    {
      const HostReg reg2 = m_state.callee_saved_order[i - 2];
      DebugAssert((m_state.host_reg_state[reg2] & (HostRegState::CalleeSaved | HostRegState::CalleeSavedAllocated)) ==
                  (HostRegState::CalleeSaved | HostRegState::CalleeSavedAllocated));

      m_code_generator.EmitPopHostRegPair(reg2, reg, i - 1);
      i -= 2;
      count += 2;

      if (commit)
      {
        m_state.host_reg_state[reg] &= ~HostRegState::CalleeSavedAllocated;
        m_state.host_reg_state[reg2] &= ~HostRegState::CalleeSavedAllocated;
      }
    }
    else
    {
      m_code_generator.EmitPopHostReg(reg, i - 1);
      if (commit)
        m_state.host_reg_state[reg] &= ~HostRegState::CalleeSavedAllocated;
      count++;
      i--;
    }
  } while (i > 0);
  if (commit)
    m_state.callee_saved_order_count = 0;

  return count;
}

void RegisterCache::ReserveCalleeSavedRegisters()
{
  for (u32 reg = 0; reg < HostReg_Count; reg++)
  {
    if ((m_state.host_reg_state[reg] & (HostRegState::CalleeSaved | HostRegState::CalleeSavedAllocated)) ==
        HostRegState::CalleeSaved)
    {
      DebugAssert(m_state.callee_saved_order_count < HostReg_Count);

      // can we find a paired register? (mainly for ARM)
      u32 reg_pair;
      for (reg_pair = reg + 1; reg_pair < HostReg_Count; reg_pair++)
      {
        if ((m_state.host_reg_state[reg_pair] & (HostRegState::CalleeSaved | HostRegState::CalleeSavedAllocated)) ==
            HostRegState::CalleeSaved)
        {
          m_code_generator.EmitPushHostRegPair(static_cast<HostReg>(reg), static_cast<HostReg>(reg_pair),
                                               GetActiveCalleeSavedRegisterCount());

          m_state.callee_saved_order[m_state.callee_saved_order_count++] = static_cast<HostReg>(reg);
          m_state.host_reg_state[reg] |= HostRegState::CalleeSavedAllocated;
          m_state.callee_saved_order[m_state.callee_saved_order_count++] = static_cast<HostReg>(reg_pair);
          m_state.host_reg_state[reg_pair] |= HostRegState::CalleeSavedAllocated;
          reg = reg_pair;
          break;
        }
      }

      if (reg_pair == HostReg_Count)
      {
        m_code_generator.EmitPushHostReg(static_cast<HostReg>(reg), GetActiveCalleeSavedRegisterCount());
        m_state.callee_saved_order[m_state.callee_saved_order_count++] = static_cast<HostReg>(reg);
        m_state.host_reg_state[reg] |= HostRegState::CalleeSavedAllocated;
      }
    }
  }
}

void RegisterCache::AssumeCalleeSavedRegistersAreSaved()
{
  for (u32 i = 0; i < HostReg_Count; i++)
  {
    if ((m_state.host_reg_state[i] & (HostRegState::CalleeSaved | HostRegState::CalleeSavedAllocated)) ==
        HostRegState::CalleeSaved)
    {
      m_state.host_reg_state[i] &= ~HostRegState::CalleeSaved;
    }
  }
}

void RegisterCache::PushState()
{
  // need to copy this manually because of the load delay values
  RegAllocState save_state;
  save_state.host_reg_state = m_state.host_reg_state;
  save_state.callee_saved_order = m_state.callee_saved_order;
  save_state.guest_reg_state = m_state.guest_reg_state;
  save_state.guest_reg_order = m_state.guest_reg_order;
  save_state.available_count = m_state.available_count;
  save_state.callee_saved_order_count = m_state.callee_saved_order_count;
  save_state.guest_reg_order_count = m_state.guest_reg_order_count;
  save_state.allocator_inhibit_count = m_state.allocator_inhibit_count;
  save_state.load_delay_register = m_state.load_delay_register;
  save_state.load_delay_value.regcache = m_state.load_delay_value.regcache;
  save_state.load_delay_value.host_reg = m_state.load_delay_value.host_reg;
  save_state.load_delay_value.size = m_state.load_delay_value.size;
  save_state.load_delay_value.flags = m_state.load_delay_value.flags;
  save_state.next_load_delay_register = m_state.next_load_delay_register;
  save_state.next_load_delay_value.regcache = m_state.next_load_delay_value.regcache;
  save_state.next_load_delay_value.host_reg = m_state.next_load_delay_value.host_reg;
  save_state.next_load_delay_value.size = m_state.next_load_delay_value.size;
  save_state.next_load_delay_value.flags = m_state.next_load_delay_value.flags;
  m_state_stack.push(std::move(save_state));
}

void RegisterCache::PopState()
{
  Assert(!m_state_stack.empty());

  // prevent destructor -> freeing of host reg
  m_state.load_delay_value.Clear();
  m_state.next_load_delay_value.Clear();

  m_state = std::move(m_state_stack.top());
  m_state_stack.pop();
}

Value RegisterCache::ReadGuestRegister(Reg guest_reg, bool cache /* = true */, bool force_host_register /* = false */,
                                       HostReg forced_host_reg /* = HostReg_Invalid */)
{
  // register zero is always zero
  if (guest_reg == Reg::zero)
  {
    // return a scratch value of zero if it's forced
    if (force_host_register)
    {
      Value temp = AllocateScratch(RegSize_32, forced_host_reg);
      m_code_generator.EmitXor(temp.host_reg, temp.host_reg, temp);
      return temp;
    }

    return Value::FromConstantU32(0);
  }

  Value& cache_value = m_state.guest_reg_state[static_cast<u8>(guest_reg)];
  if (cache_value.IsValid())
  {
    if (cache_value.IsInHostRegister())
    {
      PushRegisterToOrder(guest_reg);

      // if it's in the wrong register, return it as scratch
      if (forced_host_reg == HostReg_Invalid || cache_value.GetHostRegister() == forced_host_reg)
        return cache_value;

      Value temp = AllocateScratch(RegSize_32, forced_host_reg);
      m_code_generator.EmitCopyValue(forced_host_reg, cache_value);
      return temp;
    }
    else if (force_host_register)
    {
      // if it's not in a register, it should be constant
      DebugAssert(cache_value.IsConstant());

      HostReg host_reg;
      if (forced_host_reg == HostReg_Invalid)
      {
        host_reg = AllocateHostReg();
      }
      else
      {
        Assert(!IsHostRegInUse(forced_host_reg));
        if (!AllocateHostReg(forced_host_reg))
          Panic("Failed to allocate specific host register");
        host_reg = forced_host_reg;
      }

      Log_DebugPrintf("Allocated host register %s for constant guest register %s (0x%" PRIX64 ")",
                      m_code_generator.GetHostRegName(host_reg), GetRegName(guest_reg), cache_value.constant_value);

      m_code_generator.EmitCopyValue(host_reg, cache_value);
      cache_value.AddHostReg(this, host_reg);
      AppendRegisterToOrder(guest_reg);

      // if we're forcing a host register, we're probably going to be changing the value,
      // in which case the constant won't be correct anyway. so just drop it.
      cache_value.ClearConstant();
      return cache_value;
    }
    else
    {
      // constant
      return cache_value;
    }
  }

  HostReg host_reg;
  if (forced_host_reg == HostReg_Invalid)
  {
    host_reg = AllocateHostReg();
  }
  else
  {
    Assert(!IsHostRegInUse(forced_host_reg));
    if (!AllocateHostReg(forced_host_reg))
      Panic("Failed to allocate specific host register");
    host_reg = forced_host_reg;
  }

  m_code_generator.EmitLoadGuestRegister(host_reg, guest_reg);

  Log_DebugPrintf("Loading guest register %s to host register %s%s", GetRegName(guest_reg),
                  m_code_generator.GetHostRegName(host_reg, RegSize_32), cache ? " (cached)" : "");

  if (cache)
  {
    // Now in cache.
    cache_value.SetHostReg(this, host_reg, RegSize_32);
    AppendRegisterToOrder(guest_reg);
    return cache_value;
  }
  else
  {
    // Skip caching, return the register as a value.
    return Value::FromScratch(this, host_reg, RegSize_32);
  }
}

Value RegisterCache::ReadGuestRegisterToScratch(Reg guest_reg)
{
  HostReg host_reg = AllocateHostReg();

  Value& cache_value = m_state.guest_reg_state[static_cast<u8>(guest_reg)];
  if (cache_value.IsValid())
  {
    m_code_generator.EmitCopyValue(host_reg, cache_value);

    if (cache_value.IsConstant())
    {
      Log_DebugPrintf("Copying guest register %s from constant 0x%08X to scratch host register %s",
                      GetRegName(guest_reg), static_cast<u32>(cache_value.constant_value),
                      m_code_generator.GetHostRegName(host_reg, RegSize_32));
    }
    else
    {
      Log_DebugPrintf("Copying guest register %s from %s to scratch host register %s", GetRegName(guest_reg),
                      m_code_generator.GetHostRegName(cache_value.host_reg, RegSize_32),
                      m_code_generator.GetHostRegName(host_reg, RegSize_32));
    }
  }
  else
  {
    m_code_generator.EmitLoadGuestRegister(host_reg, guest_reg);

    Log_DebugPrintf("Loading guest register %s to scratch host register %s", GetRegName(guest_reg),
                    m_code_generator.GetHostRegName(host_reg, RegSize_32));
  }

  return Value::FromScratch(this, host_reg, RegSize_32);
}

Value RegisterCache::WriteGuestRegister(Reg guest_reg, Value&& value)
{
  // ignore writes to register zero
  DebugAssert(value.size == RegSize_32);
  if (guest_reg == Reg::zero)
    return std::move(value);

  // cancel any load delay delay
  if (m_state.load_delay_register == guest_reg)
  {
    Log_DebugPrintf("Cancelling load delay of register %s because of non-delayed write", GetRegName(guest_reg));
    m_state.load_delay_register = Reg::count;
    m_state.load_delay_value.ReleaseAndClear();
  }

  Value& cache_value = m_state.guest_reg_state[static_cast<u8>(guest_reg)];
  if (cache_value.IsInHostRegister() && value.IsInHostRegister() && cache_value.host_reg == value.host_reg)
  {
    // updating the register value.
    Log_DebugPrintf("Updating guest register %s (in host register %s)", GetRegName(guest_reg),
                    m_code_generator.GetHostRegName(value.host_reg, RegSize_32));
    cache_value = std::move(value);
    cache_value.SetDirty();
    return cache_value;
  }

  InvalidateGuestRegister(guest_reg);
  DebugAssert(!cache_value.IsValid());

  if (value.IsConstant())
  {
    // No need to allocate a host register, and we can defer the store.
    cache_value = value;
    cache_value.SetDirty();
    return cache_value;
  }

  AppendRegisterToOrder(guest_reg);

  // If it's a temporary, we can bind that to the guest register.
  if (value.IsScratch())
  {
    Log_DebugPrintf("Binding scratch register %s to guest register %s",
                    m_code_generator.GetHostRegName(value.host_reg, RegSize_32), GetRegName(guest_reg));

    cache_value = std::move(value);
    cache_value.flags &= ~ValueFlags::Scratch;
    cache_value.SetDirty();
    return Value::FromHostReg(this, cache_value.host_reg, RegSize_32);
  }

  // Allocate host register, and copy value to it.
  HostReg host_reg = AllocateHostReg();
  m_code_generator.EmitCopyValue(host_reg, value);
  cache_value.SetHostReg(this, host_reg, RegSize_32);
  cache_value.SetDirty();

  Log_DebugPrintf("Copying non-scratch register %s to %s to guest register %s",
                  m_code_generator.GetHostRegName(value.host_reg, RegSize_32),
                  m_code_generator.GetHostRegName(host_reg, RegSize_32), GetRegName(guest_reg));

  return Value::FromHostReg(this, cache_value.host_reg, RegSize_32);
}

void RegisterCache::WriteGuestRegisterDelayed(Reg guest_reg, Value&& value)
{
  // ignore writes to register zero
  DebugAssert(value.size == RegSize_32);
  if (guest_reg == Reg::zero)
    return;

  // two load delays in a row? cancel the first one.
  if (guest_reg == m_state.load_delay_register)
  {
    Log_DebugPrintf("Cancelling load delay of register %s due to new load delay", GetRegName(guest_reg));
    m_state.load_delay_register = Reg::count;
    m_state.load_delay_value.ReleaseAndClear();
  }

  // two load delay case with interpreter load delay
  m_code_generator.EmitCancelInterpreterLoadDelayForReg(guest_reg);

  // set up the load delay at the end of this instruction
  Value& cache_value = m_state.next_load_delay_value;
  Assert(m_state.next_load_delay_register == Reg::count);
  m_state.next_load_delay_register = guest_reg;

  // If it's a temporary, we can bind that to the guest register.
  if (value.IsScratch())
  {
    Log_DebugPrintf("Binding scratch register %s to load-delayed guest register %s",
                    m_code_generator.GetHostRegName(value.host_reg, RegSize_32), GetRegName(guest_reg));

    cache_value = std::move(value);
    return;
  }

  // Allocate host register, and copy value to it.
  cache_value = AllocateScratch(RegSize_32);
  m_code_generator.EmitCopyValue(cache_value.host_reg, value);

  Log_DebugPrintf("Copying non-scratch register %s to %s to load-delayed guest register %s",
                  m_code_generator.GetHostRegName(value.host_reg, RegSize_32),
                  m_code_generator.GetHostRegName(cache_value.host_reg, RegSize_32), GetRegName(guest_reg));
}

void RegisterCache::UpdateLoadDelay()
{
  // flush current load delay
  if (m_state.load_delay_register != Reg::count)
  {
    // have to clear first because otherwise it'll release the value
    Reg reg = m_state.load_delay_register;
    Value value = std::move(m_state.load_delay_value);
    m_state.load_delay_register = Reg::count;
    WriteGuestRegister(reg, std::move(value));
  }

  // next load delay -> load delay
  if (m_state.next_load_delay_register != Reg::count)
  {
    m_state.load_delay_register = m_state.next_load_delay_register;
    m_state.load_delay_value = std::move(m_state.next_load_delay_value);
    m_state.next_load_delay_register = Reg::count;
  }
}

void RegisterCache::WriteLoadDelayToCPU(bool clear)
{
  // There shouldn't be a flush at the same time as there's a new load delay.
  Assert(m_state.next_load_delay_register == Reg::count);
  if (m_state.load_delay_register != Reg::count)
  {
    Log_DebugPrintf("Flushing pending load delay of %s", GetRegName(m_state.load_delay_register));
    m_code_generator.EmitStoreInterpreterLoadDelay(m_state.load_delay_register, m_state.load_delay_value);
    if (clear)
    {
      m_state.load_delay_register = Reg::count;
      m_state.load_delay_value.ReleaseAndClear();
    }
  }
}

void RegisterCache::FlushLoadDelay(bool clear)
{
  Assert(m_state.next_load_delay_register == Reg::count);

  if (m_state.load_delay_register != Reg::count)
  {
    // if this is an exception exit, write the new value to the CPU register file, but keep it tracked for the next
    // non-exception-raised path. TODO: push/pop whole state would avoid this issue
    m_code_generator.EmitStoreGuestRegister(m_state.load_delay_register, m_state.load_delay_value);

    if (clear)
    {
      m_state.load_delay_register = Reg::count;
      m_state.load_delay_value.ReleaseAndClear();
    }
  }
}

void RegisterCache::FlushGuestRegister(Reg guest_reg, bool invalidate, bool clear_dirty)
{
  Value& cache_value = m_state.guest_reg_state[static_cast<u8>(guest_reg)];
  if (cache_value.IsDirty())
  {
    if (cache_value.IsInHostRegister())
    {
      Log_DebugPrintf("Flushing guest register %s from host register %s", GetRegName(guest_reg),
                      m_code_generator.GetHostRegName(cache_value.host_reg, RegSize_32));
    }
    else if (cache_value.IsConstant())
    {
      Log_DebugPrintf("Flushing guest register %s from constant 0x%" PRIX64, GetRegName(guest_reg),
                      cache_value.constant_value);
    }
    m_code_generator.EmitStoreGuestRegister(guest_reg, cache_value);
    if (clear_dirty)
      cache_value.ClearDirty();
  }

  if (invalidate)
    InvalidateGuestRegister(guest_reg);
}

void RegisterCache::InvalidateGuestRegister(Reg guest_reg)
{
  Value& cache_value = m_state.guest_reg_state[static_cast<u8>(guest_reg)];
  if (!cache_value.IsValid())
    return;

  if (cache_value.IsInHostRegister())
  {
    FreeHostReg(cache_value.host_reg);
    ClearRegisterFromOrder(guest_reg);
  }

  Log_DebugPrintf("Invalidating guest register %s", GetRegName(guest_reg));
  cache_value.Clear();
}

void RegisterCache::InvalidateAllNonDirtyGuestRegisters()
{
  for (u8 reg = 0; reg < static_cast<u8>(Reg::count); reg++)
  {
    Value& cache_value = m_state.guest_reg_state[reg];
    if (cache_value.IsValid() && !cache_value.IsDirty())
      InvalidateGuestRegister(static_cast<Reg>(reg));
  }
}

void RegisterCache::FlushAllGuestRegisters(bool invalidate, bool clear_dirty)
{
  for (u8 reg = 0; reg < static_cast<u8>(Reg::count); reg++)
    FlushGuestRegister(static_cast<Reg>(reg), invalidate, clear_dirty);
}

void RegisterCache::FlushCallerSavedGuestRegisters(bool invalidate, bool clear_dirty)
{
  for (u8 reg = 0; reg < static_cast<u8>(Reg::count); reg++)
  {
    const Value& gr = m_state.guest_reg_state[reg];
    if (!gr.IsInHostRegister() ||
        (m_state.host_reg_state[gr.GetHostRegister()] & HostRegState::CallerSaved) != HostRegState::CallerSaved)
    {
      continue;
    }

    FlushGuestRegister(static_cast<Reg>(reg), invalidate, clear_dirty);
  }
}

bool RegisterCache::EvictOneGuestRegister()
{
  if (m_state.guest_reg_order_count == 0)
    return false;

  // evict the register used the longest time ago
  Reg evict_reg = m_state.guest_reg_order[m_state.guest_reg_order_count - 1];
  Log_ProfilePrintf("Evicting guest register %s", GetRegName(evict_reg));
  FlushGuestRegister(evict_reg, true, true);

  return HasFreeHostRegister();
}

void RegisterCache::ClearRegisterFromOrder(Reg reg)
{
  for (u32 i = 0; i < m_state.guest_reg_order_count; i++)
  {
    if (m_state.guest_reg_order[i] == reg)
    {
      // move the registers after backwards into this spot
      const u32 count_after = m_state.guest_reg_order_count - i - 1;
      if (count_after > 0)
        std::memmove(&m_state.guest_reg_order[i], &m_state.guest_reg_order[i + 1], sizeof(Reg) * count_after);
      else
        m_state.guest_reg_order[i] = Reg::count;

      m_state.guest_reg_order_count--;
      return;
    }
  }

  Panic("Clearing register from order not in order");
}

void RegisterCache::PushRegisterToOrder(Reg reg)
{
  for (u32 i = 0; i < m_state.guest_reg_order_count; i++)
  {
    if (m_state.guest_reg_order[i] == reg)
    {
      // move the registers after backwards into this spot
      const u32 count_before = i;
      if (count_before > 0)
        std::memmove(&m_state.guest_reg_order[1], &m_state.guest_reg_order[0], sizeof(Reg) * count_before);

      m_state.guest_reg_order[0] = reg;
      return;
    }
  }

  Panic("Attempt to push register which is not ordered");
}

void RegisterCache::AppendRegisterToOrder(Reg reg)
{
  DebugAssert(m_state.guest_reg_order_count < HostReg_Count);
  if (m_state.guest_reg_order_count > 0)
    std::memmove(&m_state.guest_reg_order[1], &m_state.guest_reg_order[0], sizeof(Reg) * m_state.guest_reg_order_count);
  m_state.guest_reg_order[0] = reg;
  m_state.guest_reg_order_count++;
}

void RegisterCache::InhibitAllocation()
{
  m_state.allocator_inhibit_count++;
}

void RegisterCache::UninhibitAllocation()
{
  Assert(m_state.allocator_inhibit_count > 0);
  m_state.allocator_inhibit_count--;
}

} // namespace CPU::Recompiler
