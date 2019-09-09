#pragma once
#include "object_type_info.h"

class Object
{
  // OBJECT TYPE STUFF
private:
  static ObjectTypeInfo s_type_info;

public:
  typedef Object ThisClass;
  static const ObjectTypeInfo* StaticTypeInfo() { return &s_type_info; }
  static ObjectTypeInfo* StaticMutableTypeInfo() { return &s_type_info; }
  static const PROPERTY_DECLARATION* StaticPropertyMap() { return nullptr; }
  static ObjectFactory* StaticFactory() { return nullptr; }
  // END OBJECT TYPE STUFF

public:
  Object(const ObjectTypeInfo* type_info = &s_type_info);
  virtual ~Object();

  // Retrieves the type information for this object.
  const ObjectTypeInfo* GetTypeInfo() const { return m_type_info; }

  // Cast from one object type to another, unchecked.
  template<class T>
  const T* Cast() const
  {
    DebugAssert(m_type_info->IsDerived(T::StaticTypeInfo()));
    return static_cast<const T*>(this);
  }
  template<class T>
  T* Cast()
  {
    DebugAssert(m_type_info->IsDerived(T::StaticTypeInfo()));
    return static_cast<T*>(this);
  }

  // Cast from one object type to another, checked.
  template<class T>
  const T* SafeCast() const
  {
    return (m_type_info->IsDerived(T::StaticTypeInfo())) ? static_cast<const T*>(this) : nullptr;
  }
  template<class T>
  T* SafeCast()
  {
    return (m_type_info->IsDerived(T::StaticTypeInfo())) ? static_cast<T*>(this) : nullptr;
  }

  // Test if one object type is derived from another.
  template<class T>
  bool IsDerived() const
  {
    return (m_type_info->IsDerived(T::StaticTypeInfo()));
  }
  bool IsDerived(const ObjectTypeInfo* type) const { return (m_type_info->IsDerived(type)); }

protected:
  // Type info pointer. Set by subclasses.
  const ObjectTypeInfo* m_type_info;
};

//
// GenericObjectFactory<T>
//
template<class T>
struct GenericObjectFactory final : public ObjectFactory
{
  Object* CreateObject() override { return new T(); }
  Object* CreateObject(const String& identifier) override { return new T(); }
  void DeleteObject(Object* object) override { delete object; }
};

#define DECLARE_OBJECT_GENERIC_FACTORY(Type)                                                                           \
  \
private:                                                                                                               \
  static GenericObjectFactory<Type> s_GenericFactory;                                                                  \
  \
public:                                                                                                                \
  static ObjectFactory* StaticFactory() { return &s_GenericFactory; }

#define DEFINE_OBJECT_GENERIC_FACTORY(Type)                                                                            \
  GenericObjectFactory<Type> Type::s_GenericFactory = GenericObjectFactory<Type>();
