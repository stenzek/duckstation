#include "cheats.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string.h"
#include "common/string_util.h"
#include "cpu_core.h"
#include <cctype>
Log_SetChannel(Cheats);

using KeyValuePairVector = std::vector<std::pair<std::string, std::string>>;

CheatList::CheatList() = default;

CheatList::~CheatList() = default;

static bool IsHexCharacter(char c)
{
  return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
}

static const std::string* FindKey(const KeyValuePairVector& kvp, const char* search)
{
  for (const auto& it : kvp)
  {
    if (StringUtil::Strcasecmp(it.first.c_str(), search) == 0)
      return &it.second;
  }

  return nullptr;
}

bool CheatList::LoadFromPCSXRFile(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
    return false;

  char line[1024];
  CheatCode current_code;
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    char* start = line;
    while (*start != '\0' && std::isspace(*start))
      start++;

    // skip empty lines
    if (*start == '\0')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(*end))
    {
      *end = '\0';
      end--;
    }

    // skip comments and empty line
    if (*start == '#' || *start == ';' || *start == '/' || *start == '\"')
      continue;

    if (*start == '[' && *end == ']')
    {
      start++;
      *end = '\0';

      // new cheat
      if (current_code.Valid())
        m_codes.push_back(std::move(current_code));

      current_code = {};
      current_code.enabled = false;
      if (*start == '*')
      {
        current_code.enabled = true;
        start++;
      }

      current_code.description.append(start);
      continue;
    }

    while (!IsHexCharacter(*start) && start != end)
      start++;
    if (start == end)
      continue;

    char* end_ptr;
    CheatCode::Instruction inst;
    inst.first = static_cast<u32>(std::strtoul(start, &end_ptr, 16));
    inst.second = 0;
    if (end_ptr)
    {
      while (!IsHexCharacter(*end_ptr) && end_ptr != end)
        end_ptr++;
      if (end_ptr != end)
        inst.second = static_cast<u32>(std::strtoul(end_ptr, nullptr, 16));
    }
    current_code.instructions.push_back(inst);
  }

  if (current_code.Valid())
    m_codes.push_back(std::move(current_code));

  Log_InfoPrintf("Loaded %zu cheats from '%s' (PCSXR format)", m_codes.size(), filename);
  return !m_codes.empty();
}

bool CheatList::LoadFromLibretroFile(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
    return false;

  char line[1024];
  KeyValuePairVector kvp;
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    char* start = line;
    while (*start != '\0' && std::isspace(*start))
      start++;

    // skip empty lines
    if (*start == '\0' || *start == '=')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(*end))
    {
      *end = '\0';
      end--;
    }

    char* equals = start;
    while (*equals != '=' && equals != end)
      equals++;
    if (equals == end)
      continue;

    *equals = '\0';

    char* key_end = equals - 1;
    while (key_end > start && std::isspace(*key_end))
    {
      *key_end = '\0';
      key_end--;
    }

    char* value_start = equals + 1;
    while (*value_start != '\0' && std::isspace(*value_start))
      value_start++;

    if (value_start == end)
      continue;

    char* value_end = value_start + std::strlen(value_start) - 1;
    while (value_end > value_start && std::isspace(*value_end))
    {
      *value_end = '\0';
      value_end--;
    }

    if (value_start == value_end)
      continue;

    if (*value_start == '\"')
    {
      if (*value_end != '\"')
        continue;

      value_start++;
      *value_end = '\0';
    }

    kvp.emplace_back(start, value_start);
  }

  if (kvp.empty())
    return false;

  const std::string* num_cheats_value = FindKey(kvp, "cheats");
  const u32 num_cheats = StringUtil::FromChars<u32>(*num_cheats_value).value_or(0);
  if (num_cheats == 0)
    return false;

  for (u32 i = 0; i < num_cheats; i++)
  {
    const std::string* desc = FindKey(kvp, TinyString::FromFormat("cheat%u_desc", i));
    const std::string* code = FindKey(kvp, TinyString::FromFormat("cheat%u_code", i));
    const std::string* enable = FindKey(kvp, TinyString::FromFormat("cheat%u_enable", i));
    if (!desc || !code || !enable)
    {
      Log_WarningPrintf("Missing desc/code/enable for cheat %u in '%s'", i, filename);
      continue;
    }

    CheatCode cc;
    cc.description = *desc;
    cc.enabled = StringUtil::FromChars<bool>(*enable).value_or(false);
    if (ParseLibretroCheat(&cc, code->c_str()))
      m_codes.push_back(std::move(cc));
  }

  Log_InfoPrintf("Loaded %zu cheats from '%s' (libretro format)", m_codes.size(), filename);
  return !m_codes.empty();
}

bool CheatList::ParseLibretroCheat(CheatCode* cc, const char* line)
{
  const char* current_ptr = line;
  while (current_ptr)
  {
    char* end_ptr;
    CheatCode::Instruction inst;
    inst.first = static_cast<u32>(std::strtoul(current_ptr, &end_ptr, 16));
    current_ptr = end_ptr;
    if (end_ptr)
    {
      if (*end_ptr != ' ')
      {
        Log_WarningPrintf("Malformed code '%s'", line);
        break;
      }

      end_ptr++;
      inst.second = static_cast<u32>(std::strtoul(current_ptr, &end_ptr, 16));
      if (end_ptr && *end_ptr == '\0')
        end_ptr = nullptr;

      if (end_ptr)
      {
        if (*end_ptr != '+')
        {
          Log_WarningPrintf("Malformed code '%s'", line);
          break;
        }

        end_ptr++;
      }

      current_ptr = end_ptr;
      cc->instructions.push_back(inst);
    }
  }

  return !cc->instructions.empty();
}

void CheatList::Apply()
{
  for (const CheatCode& code : m_codes)
  {
    if (code.enabled)
      code.Apply();
  }
}

void CheatList::AddCode(CheatCode cc)
{
  m_codes.push_back(std::move(cc));
}

void CheatList::SetCode(u32 index, CheatCode cc)
{
  if (index > m_codes.size())
    return;

  if (index == m_codes.size())
  {
    m_codes.push_back(std::move(cc));
    return;
  }

  m_codes[index] = std::move(cc);
}

void CheatList::RemoveCode(u32 i)
{
  m_codes.erase(m_codes.begin() + i);
}

std::optional<CheatList::Format> CheatList::DetectFileFormat(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "rb");
  if (!fp)
    return Format::Count;

  char line[1024];
  KeyValuePairVector kvp;
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    char* start = line;
    while (*start != '\0' && std::isspace(*start))
      start++;

    // skip empty lines
    if (*start == '\0' || *start == '=')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(*end))
    {
      *end = '\0';
      end--;
    }

    if (std::strncmp(line, "cheats", 6) == 0)
      return Format::Libretro;
    else
      return Format::PCSXR;
  }

  return Format::Count;
}

bool CheatList::LoadFromFile(const char* filename, Format format)
{
  if (format == Format::Autodetect)
    format = DetectFileFormat(filename).value_or(Format::Count);

  if (format == Format::PCSXR)
    return LoadFromPCSXRFile(filename);
  else if (format == Format::Libretro)
    return LoadFromLibretroFile(filename);

  Log_ErrorPrintf("Invalid or unknown format for '%s'", filename);
  return false;
}

bool CheatList::SaveToPCSXRFile(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "wb");
  if (!fp)
    return false;

  for (const CheatCode& cc : m_codes)
  {
    std::fprintf(fp.get(), "[%s%s]\n", cc.enabled ? "*" : "", cc.description.c_str());
    for (const CheatCode::Instruction& i : cc.instructions)
      std::fprintf(fp.get(), "%08X %04X\n", i.first, i.second);
    std::fprintf(fp.get(), "\n");
  }

  std::fflush(fp.get());
  return (std::ferror(fp.get()) == 0);
}

u32 CheatList::GetEnabledCodeCount() const
{
  u32 count = 0;
  for (const CheatCode& cc : m_codes)
  {
    if (cc.enabled)
      count++;
  }

  return count;
}

void CheatList::SetCodeEnabled(u32 index, bool state)
{
  if (index >= m_codes.size())
    return;

  m_codes[index].enabled = state;
}

void CheatList::EnableCode(u32 index)
{
  SetCodeEnabled(index, true);
}

void CheatList::DisableCode(u32 index)
{
  SetCodeEnabled(index, false);
}

void CheatList::ApplyCode(u32 index)
{
  if (index >= m_codes.size())
    return;

  m_codes[index].Apply();
}

void CheatCode::Apply() const
{
  const u32 count = static_cast<u32>(instructions.size());
  u32 index = 0;
  for (; index < count;)
  {
    const Instruction& inst = instructions[index];
    switch (inst.code)
    {
      case InstructionCode::ConstantWrite8:
      {
        CPU::SafeWriteMemoryByte(inst.address, inst.value8);
        index++;
      }
      break;

      case InstructionCode::ConstantWrite16:
      {
        CPU::SafeWriteMemoryHalfWord(inst.address, inst.value16);
        index++;
      }
      break;

      case InstructionCode::Increment16:
      {
        u16 value = 0;
        CPU::SafeReadMemoryHalfWord(inst.address, &value);
        CPU::SafeWriteMemoryHalfWord(inst.address, value + 1u);
        index++;
      }
      break;

      case InstructionCode::Decrement16:
      {
        u16 value = 0;
        CPU::SafeReadMemoryHalfWord(inst.address, &value);
        CPU::SafeWriteMemoryHalfWord(inst.address, value - 1u);
        index++;
      }
      break;

      case InstructionCode::Increment8:
      {
        u8 value = 0;
        CPU::SafeReadMemoryByte(inst.address, &value);
        CPU::SafeWriteMemoryByte(inst.address, value + 1u);
        index++;
      }
      break;

      case InstructionCode::Decrement8:
      {
        u8 value = 0;
        CPU::SafeReadMemoryByte(inst.address, &value);
        CPU::SafeWriteMemoryByte(inst.address, value - 1u);
        index++;
      }
      break;

      case InstructionCode::CompareEqual16:
      {
        u16 value = 0;
        CPU::SafeReadMemoryHalfWord(inst.address, &value);
        if (value == inst.value16)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareNotEqual16:
      {
        u16 value = 0;
        CPU::SafeReadMemoryHalfWord(inst.address, &value);
        if (value != inst.value16)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareLess16:
      {
        u16 value = 0;
        CPU::SafeReadMemoryHalfWord(inst.address, &value);
        if (value < inst.value16)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareGreater16:
      {
        u16 value = 0;
        CPU::SafeReadMemoryHalfWord(inst.address, &value);
        if (value > inst.value16)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareEqual8:
      {
        u8 value = 0;
        CPU::SafeReadMemoryByte(inst.address, &value);
        if (value == inst.value8)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareNotEqual8:
      {
        u8 value = 0;
        CPU::SafeReadMemoryByte(inst.address, &value);
        if (value != inst.value8)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareLess8:
      {
        u8 value = 0;
        CPU::SafeReadMemoryByte(inst.address, &value);
        if (value < inst.value8)
          index++;
        else
          index += 2;
      }
      break;

      case InstructionCode::CompareGreater8:
      {
        u8 value = 0;
        CPU::SafeReadMemoryByte(inst.address, &value);
        if (value > inst.value8)
          index++;
        else
          index += 2;
      }
      break;

      default:
      {
        Log_ErrorPrintf("Unhandled instruction code 0x%02X (%08X %08X)", static_cast<u8>(inst.code.GetValue()),
                        inst.first, inst.second);
        index++;
      }
      break;
    }
  }
}
