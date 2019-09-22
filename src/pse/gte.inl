#include "gte.h"

template<u32 index>
u8 GTE::Core::TruncateRGB(s32 value)
{
  if (value < 0 || value > 0xFF)
  {
    if constexpr (index == 0)
      m_regs.FLAG.color_r_saturated = true;
    else if constexpr (index == 1)
      m_regs.FLAG.color_g_saturated = true;
    else
      m_regs.FLAG.color_b_saturated = true;

    value = (value < 0) ? 0 : 0xFF;
  }

  return static_cast<u8>(value);
}

template<u32 index>
s32 GTE::Core::TruncateMAC(s64 value)
{
  if (value < INT64_C(-2147483648))
  {
    if constexpr (index == 0)
      m_regs.FLAG.mac0_underflow = true;
    else if constexpr (index == 1)
      m_regs.FLAG.mac1_underflow = true;
    else if constexpr (index == 2)
      m_regs.FLAG.mac2_underflow = true;
    else if constexpr (index == 3)
      m_regs.FLAG.mac3_underflow = true;

    return static_cast<s32>(UINT32_C(0x80000000));
  }
  else if (value > INT64_C(2147483647))
  {
    if constexpr (index == 0)
      m_regs.FLAG.mac0_overflow = true;
    else if constexpr (index == 1)
      m_regs.FLAG.mac1_overflow = true;
    else if constexpr (index == 2)
      m_regs.FLAG.mac2_overflow = true;
    else if constexpr (index == 3)
      m_regs.FLAG.mac3_overflow = true;

    return static_cast<s32>(UINT32_C(0x7FFFFFFF));
  }

  return static_cast<s32>(value);
}

template<u32 index>
void GTE::Core::SetIR(s32 value, bool lm)
{
}
