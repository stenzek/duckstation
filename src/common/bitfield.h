#pragma once
#include <type_traits>

// Disable MSVC warnings that we actually handle
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4800) // warning C4800: 'int': forcing value to bool 'true' or 'false' (performance warning)
#endif

template<typename BackingDataType, typename DataType, unsigned BitIndex, unsigned BitCount>
struct BitField
{
  constexpr BitField() = default;
#ifndef _MSC_VER
  BitField& operator=(const BitField& value) = delete;
#endif

  constexpr BackingDataType GetMask() const
  {
    return ((static_cast<BackingDataType>(~0)) >> (8 * sizeof(BackingDataType) - BitCount)) << BitIndex;
  }

  operator DataType() const { return GetValue(); }

  BitField& operator=(DataType value)
  {
    SetValue(value);
    return *this;
  }

  DataType operator++()
  {
    DataType value = GetValue() + 1;
    SetValue(value);
    return GetValue();
  }

  DataType operator++(int)
  {
    DataType value = GetValue();
    SetValue(value + 1);
    return value;
  }

  DataType operator--()
  {
    DataType value = GetValue() - 1;
    SetValue(value);
    return GetValue();
  }

  DataType operator--(int)
  {
    DataType value = GetValue();
    SetValue(value - 1);
    return value;
  }

  BitField& operator+=(DataType rhs)
  {
    SetValue(GetValue() + rhs);
    return *this;
  }

  BitField& operator-=(DataType rhs)
  {
    SetValue(GetValue() - rhs);
    return *this;
  }

  BitField& operator*=(DataType rhs)
  {
    SetValue(GetValue() * rhs);
    return *this;
  }

  BitField& operator/=(DataType rhs)
  {
    SetValue(GetValue() / rhs);
    return *this;
  }

  BitField& operator^=(DataType rhs)
  {
    SetValue(GetValue() ^ rhs);
    return *this;
  }

  BitField& operator<<=(DataType rhs)
  {
    SetValue(GetValue() >> rhs);
    return *this;
  }

  BitField& operator>>=(DataType rhs)
  {
    SetValue(GetValue() >> rhs);
    return *this;
  }

  DataType GetValue() const
  {
    // TODO: Handle signed types
    if (std::is_same<DataType, bool>::value)
      return static_cast<DataType>(!!((data & GetMask()) >> BitIndex));
    else
      return static_cast<DataType>((data & GetMask()) >> BitIndex);
  }

  void SetValue(DataType value)
  {
    data &= ~GetMask();
    data |= (static_cast<BackingDataType>(value) << BitIndex) & GetMask();
  }

  BackingDataType data;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif