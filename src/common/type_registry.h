#pragma once
#include "YBaseLib/CString.h"
#include "YBaseLib/MemArray.h"
#include "YBaseLib/PODArray.h"
#include "common/types.h"

#define INVALID_TYPE_INDEX 0xFFFFFFFF

template<class T>
class TypeRegistry
{
public:
  struct RegisteredTypeInfo
  {
    T* pTypeInfo;
    const char* TypeName;
    u32 InheritanceDepth;
  };

public:
  TypeRegistry() {}
  ~TypeRegistry() {}

  u32 RegisterTypeInfo(T* pTypeInfo, const char* TypeName, u32 InheritanceDepth)
  {
    u32 Index;
    DebugAssert(pTypeInfo != nullptr);

    for (Index = 0; Index < m_arrTypes.GetSize(); Index++)
    {
      if (m_arrTypes[Index].pTypeInfo == pTypeInfo)
        Panic("Attempting to register type multiple times.");
    }

    for (Index = 0; Index < m_arrTypes.GetSize(); Index++)
    {
      if (m_arrTypes[Index].pTypeInfo == nullptr)
      {
        m_arrTypes[Index].pTypeInfo = pTypeInfo;
        m_arrTypes[Index].TypeName = TypeName;
        m_arrTypes[Index].InheritanceDepth = InheritanceDepth;
        break;
      }
    }
    if (Index == m_arrTypes.GetSize())
    {
      RegisteredTypeInfo t;
      t.pTypeInfo = pTypeInfo;
      t.TypeName = TypeName;
      t.InheritanceDepth = InheritanceDepth;
      m_arrTypes.Add(t);
    }

    CalculateMaxInheritanceDepth();
    return Index;
  }

  void UnregisterTypeInfo(T* pTypeInfo)
  {
    u32 i;
    for (i = 0; i < m_arrTypes.GetSize(); i++)
    {
      if (m_arrTypes[i].pTypeInfo == pTypeInfo)
      {
        m_arrTypes[i].pTypeInfo = nullptr;
        m_arrTypes[i].TypeName = nullptr;
        m_arrTypes[i].InheritanceDepth = 0;
        break;
      }
    }
  }

  const u32 GetNumTypes() const { return m_arrTypes.GetSize(); }
  const u32 GetMaxInheritanceDepth() const { return m_iMaxInheritanceDepth; }

  const RegisteredTypeInfo& GetRegisteredTypeInfoByIndex(u32 TypeIndex) const
  {
    return m_arrTypes.GetElement(TypeIndex);
  }

  const T* GetTypeInfoByIndex(u32 TypeIndex) const { return m_arrTypes.GetElement(TypeIndex).pTypeInfo; }

  const T* GetTypeInfoByName(const char* TypeName) const
  {
    for (u32 i = 0; i < m_arrTypes.GetSize(); i++)
    {
      if (m_arrTypes[i].pTypeInfo != nullptr && !Y_stricmp(m_arrTypes[i].TypeName, TypeName))
        return m_arrTypes[i].pTypeInfo;
    }

    return nullptr;
  }

private:
  typedef MemArray<RegisteredTypeInfo> TypeArray;
  TypeArray m_arrTypes;
  u32 m_iMaxInheritanceDepth;

  void CalculateMaxInheritanceDepth()
  {
    u32 i;
    m_iMaxInheritanceDepth = 0;

    for (i = 0; i < m_arrTypes.GetSize(); i++)
    {
      if (m_arrTypes[i].pTypeInfo != nullptr)
        m_iMaxInheritanceDepth = Max(m_iMaxInheritanceDepth, m_arrTypes[i].InheritanceDepth);
    }
  }
};
