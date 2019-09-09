#pragma once
#include "common/property.h"
#include "common/type_registry.h"
#include "common/types.h"

// Forward declare the factory type.
class Object;
struct ObjectFactory;

//
// ObjectTypeInfo
//
class ObjectTypeInfo
{
public:
  // Constants.
  static constexpr u32 INVALID_OBJECT_TYPE_INDEX = 0xFFFFFFFF;
  using RegistryType = TypeRegistry<ObjectTypeInfo>;

  // constructors
  ObjectTypeInfo(const char* TypeName, const ObjectTypeInfo* pParentTypeInfo,
                 const PROPERTY_DECLARATION* pPropertyDeclarations, ObjectFactory* pFactory);
  virtual ~ObjectTypeInfo();

  // accessors
  const u32 GetTypeIndex() const { return m_type_index; }
  const u32 GetInheritanceDepth() const { return m_inheritance_depth; }
  const char* GetTypeName() const { return m_type_name; }
  const ObjectTypeInfo* GetParentType() const { return m_parent_type; }
  ObjectFactory* GetFactory() const { return m_factory; }

  // can create?
  bool CanCreateInstance() const;
  Object* CreateInstance() const;
  void DestroyInstance(Object* obj) const;

  // type information
  // currently only does single inheritance
  bool IsDerived(const ObjectTypeInfo* type) const;

  // properties
  const PROPERTY_DECLARATION* GetPropertyDeclarationByName(const char* name) const;
  const PROPERTY_DECLARATION* GetPropertyDeclarationByIndex(u32 index) const
  {
    DebugAssert(index < m_num_property_declarations);
    return m_property_declarations[index];
  }
  u32 GetPropertyCount() const { return m_num_property_declarations; }

  // only called once.
  virtual void RegisterType();
  virtual void UnregisterType();

protected:
  u32 m_type_index;
  u32 m_inheritance_depth;
  const char* m_type_name;
  const ObjectTypeInfo* m_parent_type;
  ObjectFactory* m_factory;

  // properties
  const PROPERTY_DECLARATION* m_source_property_declarations;
  const PROPERTY_DECLARATION** m_property_declarations;
  u32 m_num_property_declarations;

  // TYPE REGISTRY
public:
  static RegistryType& GetRegistry();
  // END TYPE REGISTRY
};

//
// ObjectFactory
//
struct ObjectFactory
{
  virtual Object* CreateObject() = 0;
  virtual Object* CreateObject(const String& identifier) = 0;
  virtual void DeleteObject(Object* object) = 0;
};

// Macros
#define DECLARE_OBJECT_TYPE_INFO(Type, ParentType)                                                                     \
  \
private:                                                                                                               \
  static ObjectTypeInfo s_type_info;                                                                                   \
  \
public:                                                                                                                \
  typedef Type ThisClass;                                                                                              \
  typedef ParentType BaseClass;                                                                                        \
  static const ObjectTypeInfo* StaticTypeInfo() { return &s_type_info; }                                               \
  static ObjectTypeInfo* StaticMutableTypeInfo() { return &s_type_info; }

#define DECLARE_OBJECT_PROPERTY_MAP(Type)                                                                              \
  \
private:                                                                                                               \
  static const PROPERTY_DECLARATION s_propertyDeclarations[];                                                          \
  static const PROPERTY_DECLARATION* StaticPropertyMap() { return s_propertyDeclarations; }

#define DECLARE_OBJECT_NO_PROPERTIES(Type)                                                                             \
  \
private:                                                                                                               \
  static const PROPERTY_DECLARATION* StaticPropertyMap() { return nullptr; }

#define DEFINE_OBJECT_TYPE_INFO(Type)                                                                                  \
  ObjectTypeInfo Type::s_type_info(#Type, Type::BaseClass::StaticTypeInfo(), Type::StaticPropertyMap(),                \
                                   Type::StaticFactory())

#define DEFINE_NAMED_OBJECT_TYPE_INFO(Type, Name)                                                                      \
  ObjectTypeInfo Type::s_type_info(Name, Type::BaseClass::StaticTypeInfo(), Type::StaticPropertyMap(),                 \
                                   Type::StaticFactory())

#define DECLARE_OBJECT_NO_FACTORY(Type)                                                                                \
  \
public:                                                                                                                \
  static ObjectFactory* StaticFactory() { return nullptr; }

#define BEGIN_OBJECT_PROPERTY_MAP(Type) const PROPERTY_DECLARATION Type::s_propertyDeclarations[] = {

#define END_OBJECT_PROPERTY_MAP()                                                                                      \
  PROPERTY_TABLE_MEMBER(NULL, PROPERTY_TYPE_COUNT, 0, NULL, NULL, NULL, NULL, NULL, NULL)                              \
  }                                                                                                                    \
  ;

#define OBJECT_TYPEINFO(Type) Type::StaticTypeInfo()
#define OBJECT_TYPEINFO_PTR(Ptr) Ptr->StaticTypeInfo()

#define OBJECT_MUTABLE_TYPEINFO(Type) Type::StaticMutableTypeInfo()
#define OBJECT_MUTABLE_TYPEINFO_PTR(Type) Type->StaticMutableTypeInfo()
