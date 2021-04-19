#include "game_database.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/host_interface.h"
#include "core/system.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include <iomanip>
#include <sstream>
Log_SetChannel(GameDatabase);

GameDatabase::GameDatabase() = default;

GameDatabase::~GameDatabase()
{
  Unload();
}

bool GameDatabase::Load()
{
  // TODO: use stream directly
  std::unique_ptr<ByteStream> stream(
    g_host_interface->OpenPackageFile("database/gamedb.json", BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED));
  if (!stream)
  {
    Log_ErrorPrintf("Failed to open game database");
    return false;
  }

  std::string gamedb_data(FileSystem::ReadStreamToString(stream.get(), false));
  if (gamedb_data.empty())
  {
    Log_ErrorPrintf("Failed to read game database");
    return false;
  }

  std::unique_ptr<rapidjson::Document> json = std::make_unique<rapidjson::Document>();
  json->Parse(gamedb_data.c_str(), gamedb_data.size());
  if (json->HasParseError())
  {
    Log_ErrorPrintf("Failed to parse game database: %s at offset %zu",
                    rapidjson::GetParseError_En(json->GetParseError()), json->GetErrorOffset());
    return false;
  }

  if (!json->IsArray())
  {
    Log_ErrorPrintf("Document is not an array");
    return false;
  }

  m_json = json.release();
  return true;
}

void GameDatabase::Unload()
{
  if (m_json)
  {
    delete static_cast<rapidjson::Document*>(m_json);
    m_json = nullptr;
  }
}

static bool GetStringFromObject(const rapidjson::Value& object, const char* key, std::string* dest)
{
  dest->clear();
  auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsString())
    return false;

  dest->assign(member->value.GetString(), member->value.GetStringLength());
  return true;
}

static bool GetUIntFromObject(const rapidjson::Value& object, const char* key, u32* dest)
{
  *dest = 0;

  auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsUint())
    return false;

  *dest = member->value.GetUint();
  return true;
}

static const rapidjson::Value* FindDatabaseEntry(const std::string_view& code, rapidjson::Document* json)
{
  for (const rapidjson::Value& current : json->GetArray())
  {
    if (!current.IsObject())
    {
      Log_WarningPrintf("entry is not an object");
      continue;
    }

    auto member = current.FindMember("codes");
    if (member == current.MemberEnd())
    {
      Log_WarningPrintf("codes member is missing");
      continue;
    }

    if (!member->value.IsArray())
    {
      Log_WarningPrintf("codes is not an array");
      continue;
    }

    for (const rapidjson::Value& current_code : member->value.GetArray())
    {
      if (!current_code.IsString())
      {
        Log_WarningPrintf("code is not a string");
        continue;
      }

      if (StringUtil::Strncasecmp(current_code.GetString(), code.data(), code.length()) == 0)
        return &current;
    }
  }

  return nullptr;
}

bool GameDatabase::GetEntryForCode(const std::string_view& code, GameDatabaseEntry* entry)
{
  if (!m_json)
    return false;

  const rapidjson::Value* object = FindDatabaseEntry(code, static_cast<rapidjson::Document*>(m_json));
  if (!object)
    return false;

  if (!GetStringFromObject(*object, "serial", &entry->serial) || !GetStringFromObject(*object, "name", &entry->title))
  {
    Log_ErrorPrintf("Missing serial or title for entry");
    return false;
  }

  GetStringFromObject(*object, "genre", &entry->genre);
  GetStringFromObject(*object, "developer", &entry->developer);
  GetStringFromObject(*object, "publisher", &entry->publisher);

  GetUIntFromObject(*object, "minPlayers", &entry->min_players);
  GetUIntFromObject(*object, "maxPlayers", &entry->max_players);
  GetUIntFromObject(*object, "minBlocks", &entry->min_blocks);
  GetUIntFromObject(*object, "maxBlocks", &entry->max_blocks);

  entry->release_date = 0;
  {
    std::string release_date;
    if (GetStringFromObject(*object, "releaseDate", &release_date))
    {
      std::istringstream iss(release_date);
      struct tm parsed_time = {};
      iss >> std::get_time(&parsed_time, "%Y-%m-%d");
      if (!iss.fail())
      {
        parsed_time.tm_isdst = 0;
#ifdef _WIN32
        entry->release_date = _mkgmtime(&parsed_time);
#else
        entry->release_date = timegm(&parsed_time);
#endif
      }
    }
  }

  entry->supported_controllers_mask = ~0u;
  auto controllers = object->FindMember("controllers");
  if (controllers != object->MemberEnd())
  {
    if (controllers->value.IsArray())
    {
      bool first = true;
      for (const rapidjson::Value& controller : controllers->value.GetArray())
      {
        if (!controller.IsString())
        {
          Log_WarningPrintf("controller is not a string");
          return false;
        }

        std::optional<ControllerType> ctype = Settings::ParseControllerTypeName(controller.GetString());
        if (!ctype.has_value())
        {
          Log_WarningPrintf("Invalid controller type '%s'", controller.GetString());
          return false;
        }

        if (first)
        {
          entry->supported_controllers_mask = 0;
          first = false;
        }

        entry->supported_controllers_mask |= (1u << static_cast<u32>(ctype.value()));
      }
    }
    else
    {
      Log_WarningPrintf("controllers is not an array");
    }
  }

  return true;
}

bool GameDatabase::GetEntryForDisc(CDImage* image, GameDatabaseEntry* entry)
{
  std::string exe_name_code(System::GetGameCodeForImage(image, false));
  if (!exe_name_code.empty() && GetEntryForCode(exe_name_code, entry))
    return true;

  std::string exe_hash_code(System::GetGameHashCodeForImage(image));
  if (!exe_hash_code.empty() && GetEntryForCode(exe_hash_code, entry))
    return true;

  Log_WarningPrintf("No entry found for disc (exe code: '%s', hash code: '%s')", exe_name_code.c_str(),
                    exe_hash_code.c_str());
  return false;
}
