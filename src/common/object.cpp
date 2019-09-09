#include "common/object.h"

// Have to define this manually as Object has no parent class.
ObjectTypeInfo Object::s_type_info("Object", nullptr, nullptr, nullptr);

Object::Object(const ObjectTypeInfo* pObjectTypeInfo /* = &s_typeInfo */) : m_type_info(pObjectTypeInfo) {}

Object::~Object() = default;
