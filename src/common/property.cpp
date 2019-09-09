#include "common/property.h"
#include "YBaseLib/BinaryReader.h"
#include "YBaseLib/BinaryWriter.h"
#include "YBaseLib/StringConverter.h"

bool GetPropertyValueAsString(const void* object, const PROPERTY_DECLARATION* property, String& value)
{
  if (!property->GetPropertyCallback)
    return false;

  // Strings handled seperately.
  if (property->Type == PROPERTY_TYPE_STRING)
  {
    // We can pass StrValue directly across.
    return property->GetPropertyCallback(object, property->pGetPropertyCallbackUserData, &value);
  }
  else
  {
    // 32 bytes should be enough for the actual value. (largest is currently transform, which is float3 + quat + float)
    byte TempValue[32];

    // Call the function.
    if (!property->GetPropertyCallback(object, property->pGetPropertyCallbackUserData, &TempValue))
      return false;

    // Now stringize it based on type.
    switch (property->Type)
    {
      case PROPERTY_TYPE_BOOL:
        StringConverter::BoolToString(value, reinterpret_cast<const bool&>(TempValue));
        break;

      case PROPERTY_TYPE_UINT:
        StringConverter::UInt32ToString(value, reinterpret_cast<const u32&>(TempValue));
        break;

      case PROPERTY_TYPE_INT:
        StringConverter::Int32ToString(value, reinterpret_cast<const s32&>(TempValue));
        break;

      case PROPERTY_TYPE_FLOAT:
        StringConverter::FloatToString(value, reinterpret_cast<const float&>(TempValue));
        break;

      default:
        UnreachableCode();
        break;
    }

    return true;
  }
}

bool SetPropertyValueFromString(void* object, const PROPERTY_DECLARATION* property, const char* value)
{
  if (property->SetPropertyCallback == NULL)
    return false;

  // Strings handled seperately.
  if (property->Type == PROPERTY_TYPE_STRING)
  {
    // Create a constant string.
    StaticString StringRef(value);
    if (!property->SetPropertyCallback(object, property->pSetPropertyCallbackUserData, &StringRef))
      return false;
  }
  else
  {
    // 32 bytes should be enough for the actual value. (largest is currently transform, which is float3 + quat + float)
    byte TempValue[32];

    // Un-stringize based on type.
    switch (property->Type)
    {
      case PROPERTY_TYPE_BOOL:
        reinterpret_cast<bool&>(TempValue) = StringConverter::StringToBool(value);
        break;

      case PROPERTY_TYPE_UINT:
        reinterpret_cast<u32&>(TempValue) = StringConverter::StringToUInt32(value);
        break;

      case PROPERTY_TYPE_INT:
        reinterpret_cast<s32&>(TempValue) = StringConverter::StringToInt32(value);
        break;

      case PROPERTY_TYPE_FLOAT:
        reinterpret_cast<float&>(TempValue) = StringConverter::StringToFloat(value);
        break;

      default:
        UnreachableCode();
        break;
    }

    // Call the function.
    if (!property->SetPropertyCallback(object, property->pSetPropertyCallbackUserData, TempValue))
      return false;
  }

  // Notify updater if needed.
  // if (pProperty->PropertyChangedCallback != NULL)
  // pProperty->PropertyChangedCallback(pObject, pProperty->pPropertyChangedCallbackUserData);

  return true;
}

bool WritePropertyValueToBuffer(const void* object, const PROPERTY_DECLARATION* property, BinaryWriter& writer)
{
  if (!property->GetPropertyCallback)
    return false;

  // Strings handled seperately.
  if (property->Type == PROPERTY_TYPE_STRING)
  {
    // We can pass StrValue directly across.
    SmallString stringValue;
    if (!property->GetPropertyCallback(object, property->pGetPropertyCallbackUserData, &stringValue))
      return false;

    writer.WriteUInt32(stringValue.GetLength() + 1);
    writer.WriteCString(stringValue);
    return true;
  }
  else
  {
    // 32 bytes should be enough for the actual value. (largest is currently transform, which is float3 + quat + float)
    byte TempValue[32];

    // Call the function.
    if (!property->GetPropertyCallback(object, property->pGetPropertyCallbackUserData, &TempValue))
      return false;

    // Now stringize it based on type.
    switch (property->Type)
    {
      case PROPERTY_TYPE_BOOL:
        writer.WriteUInt32(1);
        writer.WriteBool(reinterpret_cast<const bool&>(TempValue));
        break;

      case PROPERTY_TYPE_UINT:
        writer.WriteUInt32(4);
        writer.WriteUInt32(reinterpret_cast<const u32&>(TempValue));
        break;

      case PROPERTY_TYPE_INT:
        writer.WriteUInt32(4);
        writer.WriteInt32(reinterpret_cast<const s32&>(TempValue));
        break;

      case PROPERTY_TYPE_FLOAT:
        writer.WriteUInt32(4);
        writer.WriteFloat(reinterpret_cast<const float&>(TempValue));
        break;

      default:
        UnreachableCode();
        break;
    }

    return true;
  }
}

bool ReadPropertyValueFromBuffer(void* object, const PROPERTY_DECLARATION* property, BinaryReader& reader)
{
  if (!property->SetPropertyCallback)
    return false;

  // Strings handled seperately.
  if (property->Type == PROPERTY_TYPE_STRING)
  {
    u32 stringLength = reader.ReadUInt32();

    SmallString stringValue;
    reader.ReadCString(stringValue);
    if (stringValue.GetLength() != (stringLength - 1) ||
        !property->SetPropertyCallback(object, property->pSetPropertyCallbackUserData, &stringValue))
      return false;
  }
  else
  {
    // 32 bytes should be enough for the actual value. (largest is currently transform, which is float3 + quat + float)
    byte temp_value[32];

    // Un-stringize based on type.
    switch (property->Type)
    {
      case PROPERTY_TYPE_BOOL:
        if (reader.ReadUInt32() != 1)
        {
          return false;
        }
        reinterpret_cast<bool&>(temp_value) = reader.ReadBool();
        break;

      case PROPERTY_TYPE_UINT:
        if (reader.ReadUInt32() != 4)
        {
          return false;
        }
        reinterpret_cast<u32&>(temp_value) = reader.ReadUInt32();
        break;

      case PROPERTY_TYPE_INT:
        if (reader.ReadUInt32() != 4)
        {
          return false;
        }
        reinterpret_cast<s32&>(temp_value) = reader.ReadInt32();
        break;

      case PROPERTY_TYPE_FLOAT:
        if (reader.ReadUInt32() != 4)
        {
          return false;
        }
        reinterpret_cast<float&>(temp_value) = reader.ReadFloat();
        break;

      default:
        UnreachableCode();
        break;
    }

    // Call the function.
    if (!property->SetPropertyCallback(object, property->pSetPropertyCallbackUserData, temp_value))
      return false;
  }

  // Notify updater if needed.
  // if (pProperty->PropertyChangedCallback != NULL)
  // pProperty->PropertyChangedCallback(pObject, pProperty->pPropertyChangedCallbackUserData);

  return true;
}

bool EncodePropertyTypeToBuffer(PROPERTY_TYPE type, const char* value_string, BinaryWriter& writer)
{
  // Strings handled seperately.
  if (type == PROPERTY_TYPE_STRING)
  {
    // We can pass StrValue directly across.
    writer.WriteUInt32(Y_strlen(value_string) + 1);
    writer.WriteCString(value_string);
    return true;
  }
  else
  {
    // Now stringize it based on type.
    switch (type)
    {
      case PROPERTY_TYPE_BOOL:
        writer.WriteUInt32(1);
        writer.WriteBool(StringConverter::StringToBool(value_string));
        break;

      case PROPERTY_TYPE_UINT:
        writer.WriteUInt32(4);
        writer.WriteUInt32(StringConverter::StringToUInt32(value_string));
        break;

      case PROPERTY_TYPE_INT:
        writer.WriteUInt32(4);
        writer.WriteInt32(StringConverter::StringToInt32(value_string));
        break;

      case PROPERTY_TYPE_FLOAT:
        writer.WriteUInt32(4);
        writer.WriteFloat(StringConverter::StringToFloat(value_string));
        break;

      default:
        UnreachableCode();
        break;
    }

    return true;
  }
}

// default property callbacks
bool DefaultPropertyTableCallbacks::GetBool(const void* pObjectPtr, const void* pUserData, bool* pValuePtr)
{
  *pValuePtr = *((const bool*)((((const byte*)pObjectPtr) + (*(int*)&pUserData))));
  return true;
}

bool DefaultPropertyTableCallbacks::SetBool(void* pObjectPtr, const void* pUserData, const bool* pValuePtr)
{
  *((bool*)((((byte*)pObjectPtr) + (*(int*)&pUserData)))) = *pValuePtr;
  return true;
}

bool DefaultPropertyTableCallbacks::GetUInt(const void* pObjectPtr, const void* pUserData, u32* pValuePtr)
{
  *pValuePtr = *((const u32*)((((const byte*)pObjectPtr) + (*(u32*)&pUserData))));
  return true;
}

bool DefaultPropertyTableCallbacks::SetUInt(void* pObjectPtr, const void* pUserData, const u32* pValuePtr)
{
  *((u32*)((((byte*)pObjectPtr) + (*(u32*)&pUserData)))) = *pValuePtr;
  return true;
}

bool DefaultPropertyTableCallbacks::GetInt(const void* pObjectPtr, const void* pUserData, s32* pValuePtr)
{
  *pValuePtr = *((const s32*)((((const byte*)pObjectPtr) + (*(s32*)&pUserData))));
  return true;
}

bool DefaultPropertyTableCallbacks::SetInt(void* pObjectPtr, const void* pUserData, const s32* pValuePtr)
{
  *((s32*)((((byte*)pObjectPtr) + (*(s32*)&pUserData)))) = *pValuePtr;
  return true;
}

bool DefaultPropertyTableCallbacks::GetFloat(const void* pObjectPtr, const void* pUserData, float* pValuePtr)
{
  *pValuePtr = *((const float*)((((const byte*)pObjectPtr) + (*(int*)&pUserData))));
  return true;
}

bool DefaultPropertyTableCallbacks::SetFloat(void* pObjectPtr, const void* pUserData, const float* pValuePtr)
{
  *((float*)((((byte*)pObjectPtr) + (*(int*)&pUserData)))) = *pValuePtr;
  return true;
}

bool DefaultPropertyTableCallbacks::SetString(void* pObjectPtr, const void* pUserData, const String* pValuePtr)
{
  ((String*)((((byte*)pObjectPtr) + (*(int*)&pUserData))))->Assign(*pValuePtr);
  return true;
}

bool DefaultPropertyTableCallbacks::GetString(const void* pObjectPtr, const void* pUserData, String* pValuePtr)
{
  pValuePtr->Assign(*((const String*)((((const byte*)pObjectPtr) + (*(int*)&pUserData)))));
  return true;
}

bool DefaultPropertyTableCallbacks::GetConstBool(const void* pObjectPtr, const void* pUserData, bool* pValuePtr)
{
  bool Value = (pUserData != 0) ? true : false;
  *pValuePtr = Value;
  return true;
}
