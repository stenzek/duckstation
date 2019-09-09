#pragma once
#include "common/types.h"

class BinaryReader;
class BinaryWriter;
class String;

enum : u32
{
  MAX_PROPERTY_TABLE_NAME_LENGTH = 128,
  MAX_PROPERTY_NAME_LENGTH = 128
};

enum PROPERTY_TYPE
{
  PROPERTY_TYPE_BOOL,
  PROPERTY_TYPE_UINT,
  PROPERTY_TYPE_INT,
  PROPERTY_TYPE_FLOAT,
  PROPERTY_TYPE_STRING,
  PROPERTY_TYPE_COUNT,
};

enum PROPERTY_FLAG
{
  PROPERTY_FLAG_READ_ONLY = (1 << 0), // Property cannot be modified by user. Engine can still modify it, however.
  PROPERTY_FLAG_INVOKE_CHANGE_CALLBACK_ON_CREATE =
    (1 << 1), // Property change callback will be invoked when the object is being created. By default it is not.
};

struct PROPERTY_DECLARATION
{
  typedef bool (*GET_PROPERTY_CALLBACK)(const void* object, const void* userdata, void* value_ptr);
  typedef bool (*SET_PROPERTY_CALLBACK)(void* object, const void* userdata, const void* value_ptr);
  typedef void (*PROPERTY_CHANGED_CALLBACK)(void* object, const void* userdata);

  const char* Name;
  PROPERTY_TYPE Type;
  u32 Flags;

  GET_PROPERTY_CALLBACK GetPropertyCallback;
  const void* pGetPropertyCallbackUserData;
  SET_PROPERTY_CALLBACK SetPropertyCallback;
  const void* pSetPropertyCallbackUserData;
  PROPERTY_CHANGED_CALLBACK PropertyChangedCallback;
  const void* pPropertyChangedCallbackUserData;
};

bool GetPropertyValueAsString(const void* object, const PROPERTY_DECLARATION* property, String& value);
bool SetPropertyValueFromString(void* object, const PROPERTY_DECLARATION* property, const char* value);
bool WritePropertyValueToBuffer(const void* object, const PROPERTY_DECLARATION* property, BinaryWriter& writer);
bool ReadPropertyValueFromBuffer(void* object, const PROPERTY_DECLARATION* property, BinaryReader& reader);
bool EncodePropertyTypeToBuffer(PROPERTY_TYPE type, const char* value_string, BinaryWriter& writer);

namespace DefaultPropertyTableCallbacks {
// builtin functions
bool GetBool(const void* object, const void* userdata, bool* value_ptr);
bool SetBool(void* object, const void* userdata, const bool* value_ptr);
bool GetUInt(const void* object, const void* userdata, u32* value_ptr);
bool SetUInt(void* object, const void* userdata, const u32* value_ptr);
bool GetInt(const void* object, const void* userdata, s32* value_ptr);
bool SetInt(void* object, const void* userdata, const s32* value_ptr);
bool GetFloat(const void* object, const void* userdata, float* value_ptr);
bool SetFloat(void* object, const void* userdata, const float* value_ptr);
bool GetString(const void* object, const void* userdata, String* value_ptr);
bool SetString(void* object, const void* userdata, const String* value_ptr);

// static bool value
bool GetConstBool(const void* object, const void* userdata, bool* value_ptr);
} // namespace DefaultPropertyTableCallbacks

#define PROPERTY_TABLE_MEMBER(Name, Type, Flags, GetPropertyCallback, GetPropertyCallbackUserData,                     \
                              SetPropertyCallback, SetPropertyCallbackUserData, PropertyChangedCallback,               \
                              PropertyChangedCallbackUserData)                                                         \
  {Name,                                                                                                               \
   Type,                                                                                                               \
   Flags,                                                                                                              \
   (PROPERTY_DECLARATION::GET_PROPERTY_CALLBACK)(GetPropertyCallback),                                                 \
   (const void*)(GetPropertyCallbackUserData),                                                                         \
   (PROPERTY_DECLARATION::SET_PROPERTY_CALLBACK)(SetPropertyCallback),                                                 \
   (const void*)(SetPropertyCallbackUserData),                                                                         \
   (PROPERTY_DECLARATION::PROPERTY_CHANGED_CALLBACK)(PropertyChangedCallback),                                         \
   (const void*)(PropertyChangedCallbackUserData)},

#define PROPERTY_TABLE_MEMBER_BOOL(Name, Flags, Offset, ChangedFunc, ChangedFuncUserData)                              \
  PROPERTY_TABLE_MEMBER(Name, PROPERTY_TYPE_BOOL, Flags, DefaultPropertyTableCallbacks::GetBool, (Offset),             \
                        DefaultPropertyTableCallbacks::SetBool, (Offset), ChangedFunc, ChangedFuncUserData)

#define PROPERTY_TABLE_MEMBER_UINT(Name, Flags, Offset, ChangedFunc, ChangedFuncUserData)                              \
  PROPERTY_TABLE_MEMBER(Name, PROPERTY_TYPE_INT, Flags, DefaultPropertyTableCallbacks::GetUInt, (Offset),              \
                        DefaultPropertyTableCallbacks::SetUInt, (Offset), ChangedFunc, ChangedFuncUserData)

#define PROPERTY_TABLE_MEMBER_INT(Name, Flags, Offset, ChangedFunc, ChangedFuncUserData)                               \
  PROPERTY_TABLE_MEMBER(Name, PROPERTY_TYPE_INT, Flags, DefaultPropertyTableCallbacks::GetInt, (Offset),               \
                        DefaultPropertyTableCallbacks::SetInt, (Offset), ChangedFunc, ChangedFuncUserData)

#define PROPERTY_TABLE_MEMBER_FLOAT(Name, Flags, Offset, ChangedFunc, ChangedFuncUserData)                             \
  PROPERTY_TABLE_MEMBER(Name, PROPERTY_TYPE_FLOAT, Flags, DefaultPropertyTableCallbacks::GetFloat, (Offset),           \
                        DefaultPropertyTableCallbacks::SetFloat, (Offset), ChangedFunc, ChangedFuncUserData)

#define PROPERTY_TABLE_MEMBER_STRING(Name, Flags, Offset, ChangedFunc, ChangedFuncUserData)                            \
  PROPERTY_TABLE_MEMBER(Name, PROPERTY_TYPE_STRING, Flags, DefaultPropertyTableCallbacks::GetString, (Offset),         \
                        DefaultPropertyTableCallbacks::SetString, (Offset), ChangedFunc, ChangedFuncUserData)
