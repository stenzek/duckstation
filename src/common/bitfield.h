#pragma once
#include "types.h"
#include <type_traits>

// Disable MSVC warnings that we actually handle
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4800) // warning C4800: 'int': forcing value to bool 'true' or 'false' (performance warning)
#endif

template<typename BackingDataType, typename DataType, unsigned BitIndex, unsigned BitCount>
struct BitField
{
  static_assert(!std::is_same_v<DataType, bool> || BitCount == 1, "Boolean bitfields should only be 1 bit");

  // We have to delete the copy assignment operator otherwise we can't use this class in anonymous structs/unions.
  BitField& operator=(const BitField& rhs) = delete;

  ALWAYS_INLINE constexpr BackingDataType GetMask() const
  {
    return ((static_cast<BackingDataType>(~0)) >> (8 * sizeof(BackingDataType) - BitCount)) << BitIndex;
  }

  ALWAYS_INLINE constexpr operator DataType() const { return GetValue(); }

  ALWAYS_INLINE constexpr BitField& operator=(DataType value)
  {
    SetValue(value);
    return *this;
  }

  ALWAYS_INLINE constexpr DataType operator++()
  {
    DataType value = GetValue() + 1;
    SetValue(value);
    return GetValue();
  }

  ALWAYS_INLINE constexpr DataType operator++(int)
  {
    DataType value = GetValue();
    SetValue(value + 1);
    return value;
  }

  ALWAYS_INLINE constexpr DataType operator--()
  {
    DataType value = GetValue() - 1;
    SetValue(value);
    return GetValue();
  }

  ALWAYS_INLINE constexpr DataType operator--(int)
  {
    DataType value = GetValue();
    SetValue(value - 1);
    return value;
  }

  ALWAYS_INLINE constexpr BitField& operator+=(DataType rhs)
  {
    SetValue(GetValue() + rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator-=(DataType rhs)
  {
    SetValue(GetValue() - rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator*=(DataType rhs)
  {
    SetValue(GetValue() * rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator/=(DataType rhs)
  {
    SetValue(GetValue() / rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator&=(DataType rhs)
  {
    SetValue(GetValue() & rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator|=(DataType rhs)
  {
    SetValue(GetValue() | rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator^=(DataType rhs)
  {
    SetValue(GetValue() ^ rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator<<=(DataType rhs)
  {
    SetValue(GetValue() << rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr BitField& operator>>=(DataType rhs)
  {
    SetValue(GetValue() >> rhs);
    return *this;
  }

  ALWAYS_INLINE constexpr DataType GetValue() const
  {
    if constexpr (std::is_same_v<DataType, bool>)
    {
      return static_cast<DataType>(!!((data & GetMask()) >> BitIndex));
    }
    else if constexpr (std::is_signed_v<DataType>)
    {
      constexpr int shift = 8 * sizeof(DataType) - BitCount;
      return (static_cast<DataType>(data >> BitIndex) << shift) >> shift;
    }
    else
    {
      return static_cast<DataType>((data & GetMask()) >> BitIndex);
    }
  }

  ALWAYS_INLINE constexpr void SetValue(DataType value)
  {
    data = (data & ~GetMask()) | ((static_cast<BackingDataType>(value) << BitIndex) & GetMask());
  }

  BackingDataType data;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
