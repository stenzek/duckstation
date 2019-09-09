#include "common/object_type_info.h"

static ObjectTypeInfo::RegistryType s_registry;

ObjectTypeInfo::RegistryType& ObjectTypeInfo::GetRegistry()
{
  return s_registry;
}

ObjectTypeInfo::ObjectTypeInfo(const char* TypeName, const ObjectTypeInfo* pParentTypeInfo,
                               const PROPERTY_DECLARATION* pPropertyDeclarations, ObjectFactory* pFactory)
  : m_type_index(INVALID_OBJECT_TYPE_INDEX), m_inheritance_depth(0), m_type_name(TypeName),
    m_parent_type(pParentTypeInfo), m_factory(pFactory), m_source_property_declarations(pPropertyDeclarations),
    m_property_declarations(nullptr), m_num_property_declarations(0)
{
}

ObjectTypeInfo::~ObjectTypeInfo()
{
  // DebugAssert(m_iTypeIndex == INVALID_TYPE_INDEX);
}

bool ObjectTypeInfo::CanCreateInstance() const
{
  return (m_factory != nullptr);
}

Object* ObjectTypeInfo::CreateInstance() const
{
  DebugAssert(m_factory != nullptr);
  return m_factory->CreateObject();
}

void ObjectTypeInfo::DestroyInstance(Object* obj) const
{
  DebugAssert(m_factory != nullptr);
  m_factory->DeleteObject(obj);
}

bool ObjectTypeInfo::IsDerived(const ObjectTypeInfo* pTypeInfo) const
{
  const ObjectTypeInfo* current_type = this;
  do
  {
    if (current_type == pTypeInfo)
      return true;

    current_type = current_type->m_parent_type;
  } while (current_type != nullptr);

  return false;
}

const PROPERTY_DECLARATION* ObjectTypeInfo::GetPropertyDeclarationByName(const char* PropertyName) const
{
  for (u32 i = 0; i < m_num_property_declarations; i++)
  {
    if (!Y_stricmp(m_property_declarations[i]->Name, PropertyName))
      return m_property_declarations[i];
  }

  return nullptr;
}

void ObjectTypeInfo::RegisterType()
{
  if (m_type_index != INVALID_OBJECT_TYPE_INDEX)
    return;

  // our stuff
  const ObjectTypeInfo* pCurrentTypeInfo;
  const PROPERTY_DECLARATION* pPropertyDeclaration;

  // get property count
  pCurrentTypeInfo = this;
  m_num_property_declarations = 0;
  m_inheritance_depth = 0;
  while (pCurrentTypeInfo != nullptr)
  {
    if (pCurrentTypeInfo->m_source_property_declarations != nullptr)
    {
      pPropertyDeclaration = pCurrentTypeInfo->m_source_property_declarations;
      while (pPropertyDeclaration->Name != nullptr)
      {
        m_num_property_declarations++;
        pPropertyDeclaration++;
      }
    }

    pCurrentTypeInfo = pCurrentTypeInfo->GetParentType();
    m_inheritance_depth++;
  }

  if (m_num_property_declarations > 0)
  {
    m_property_declarations = new const PROPERTY_DECLARATION*[m_num_property_declarations];
    pCurrentTypeInfo = this;
    u32 i = 0;
    while (pCurrentTypeInfo != nullptr)
    {
      if (pCurrentTypeInfo->m_source_property_declarations != nullptr)
      {
        pPropertyDeclaration = pCurrentTypeInfo->m_source_property_declarations;
        while (pPropertyDeclaration->Name != nullptr)
        {
          DebugAssert(i < m_num_property_declarations);
          m_property_declarations[i++] = pPropertyDeclaration++;
        }
      }

      pCurrentTypeInfo = pCurrentTypeInfo->GetParentType();
    }
  }

  m_type_index = GetRegistry().RegisterTypeInfo(this, m_type_name, m_inheritance_depth);
}

void ObjectTypeInfo::UnregisterType()
{
  if (m_type_index == INVALID_OBJECT_TYPE_INDEX)
    return;

  delete[] m_property_declarations;
  m_property_declarations = nullptr;
  m_num_property_declarations = 0;

  m_type_index = INVALID_OBJECT_TYPE_INDEX;
  GetRegistry().UnregisterTypeInfo(this);
}
