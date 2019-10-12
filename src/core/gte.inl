#include "gte.h"

template<u32 index>
void GTE::Core::CheckMACOverflow(s64 value)
{
  constexpr s64 MIN_VALUE = (index == 0) ? MAC0_MIN_VALUE : MAC123_MIN_VALUE;
  constexpr s64 MAX_VALUE = (index == 0) ? MAC0_MAX_VALUE : MAC123_MAX_VALUE;
  if (value < MIN_VALUE)
  {
    if constexpr (index == 0)
      m_regs.FLAG.mac0_underflow = true;
    else if constexpr (index == 1)
      m_regs.FLAG.mac1_underflow = true;
    else if constexpr (index == 2)
      m_regs.FLAG.mac2_underflow = true;
    else if constexpr (index == 3)
      m_regs.FLAG.mac3_underflow = true;
  }
  else if (value > MAX_VALUE)
  {
    if constexpr (index == 0)
      m_regs.FLAG.mac0_overflow = true;
    else if constexpr (index == 1)
      m_regs.FLAG.mac1_overflow = true;
    else if constexpr (index == 2)
      m_regs.FLAG.mac2_overflow = true;
    else if constexpr (index == 3)
      m_regs.FLAG.mac3_overflow = true;
  }
}

template<u32 index>
s64 GTE::Core::SignExtendMACResult(s64 value)
{
  CheckMACOverflow<index>(value);
  return SignExtendN < index == 0 ? 31 : 44 > (value);
}

template<u32 index>
void GTE::Core::TruncateAndSetMAC(s64 value, u8 shift)
{
  CheckMACOverflow<index>(value);

  // shift should be done before storing to avoid losing precision
  value >>= shift;

  m_regs.dr32[24 + index] = Truncate32(static_cast<u64>(value));
}

template<u32 index>
void GTE::Core::TruncateAndSetIR(s32 value, bool lm)
{
  constexpr s32 MIN_VALUE = (index == 0) ? IR0_MIN_VALUE : IR123_MIN_VALUE;
  constexpr s32 MAX_VALUE = (index == 0) ? IR0_MAX_VALUE : IR123_MAX_VALUE;
  const s32 actual_min_value = lm ? 0 : MIN_VALUE;
  if (value < actual_min_value)
  {
    value = actual_min_value;
    if constexpr (index == 0)
      m_regs.FLAG.ir0_saturated = true;
    else if constexpr (index == 1)
      m_regs.FLAG.ir1_saturated = true;
    else if constexpr (index == 2)
      m_regs.FLAG.ir2_saturated = true;
    else if constexpr (index == 3)
      m_regs.FLAG.ir3_saturated = true;
  }
  else if (value > MAX_VALUE)
  {
    value = MAX_VALUE;
    if constexpr (index == 0)
      m_regs.FLAG.ir0_saturated = true;
    else if constexpr (index == 1)
      m_regs.FLAG.ir1_saturated = true;
    else if constexpr (index == 2)
      m_regs.FLAG.ir2_saturated = true;
    else if constexpr (index == 3)
      m_regs.FLAG.ir3_saturated = true;
  }

  // store sign-extended 16-bit value as 32-bit
  m_regs.dr32[8 + index] = value;
}

template<u32 index>
void GTE::Core::TruncateAndSetMACAndIR(s64 value, u8 shift, bool lm)
{
  CheckMACOverflow<index>(value);

  // shift should be done before storing to avoid losing precision
  value >>= shift;

  // set MAC
  const s32 value32 = static_cast<s32>(value);
  m_regs.dr32[24 + index] = value32;

  // set IR
  TruncateAndSetIR<index>(value32, lm);
}

template<u32 index>
u32 GTE::Core::TruncateRGB(s32 value)
{
  if (value < 0 || value > 0xFF)
  {
    if constexpr (index == 0)
      m_regs.FLAG.color_r_saturated = true;
    else if constexpr (index == 1)
      m_regs.FLAG.color_g_saturated = true;
    else
      m_regs.FLAG.color_b_saturated = true;

    return (value < 0) ? 0 : 0xFF;
  }

  return static_cast<u32>(value);
}
