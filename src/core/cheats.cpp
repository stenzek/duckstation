#include "cheats.h"
#include "bus.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string.h"
#include "common/string_util.h"
#include "controller.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "host_interface.h"
#include "system.h"
#include <cctype>
#include <iomanip>
#include <sstream>
#include <type_traits>
Log_SetChannel(Cheats);
static std::array<u32, 256> cht_register; // Used for D7 ,51 & 52 cheat types

using KeyValuePairVector = std::vector<std::pair<std::string, std::string>>;

static bool IsValidScanAddress(PhysicalMemoryAddress address)
{
  if ((address & CPU::DCACHE_LOCATION_MASK) == CPU::DCACHE_LOCATION &&
      (address & CPU::DCACHE_OFFSET_MASK) < CPU::DCACHE_SIZE)
  {
    return true;
  }

  address &= CPU::PHYSICAL_MEMORY_ADDRESS_MASK;

  if (address < Bus::RAM_MIRROR_END)
    return true;

  if (address >= Bus::BIOS_BASE && address < (Bus::BIOS_BASE + Bus::BIOS_SIZE))
    return true;

  return false;
}

template<typename T>
static T DoMemoryRead(VirtualMemoryAddress address)
{
  using UnsignedType = typename std::make_unsigned_t<T>;
  static_assert(std::is_same_v<UnsignedType, u8> || std::is_same_v<UnsignedType, u16> ||
                std::is_same_v<UnsignedType, u32>);

  T result;
  if constexpr (std::is_same_v<UnsignedType, u8>)
    return CPU::SafeReadMemoryByte(address, &result) ? result : static_cast<T>(0);
  else if constexpr (std::is_same_v<UnsignedType, u16>)
    return CPU::SafeReadMemoryHalfWord(address, &result) ? result : static_cast<T>(0);
  else // if constexpr (std::is_same_v<UnsignedType, u32>)
    return CPU::SafeReadMemoryWord(address, &result) ? result : static_cast<T>(0);
}

template<typename T>
static void DoMemoryWrite(PhysicalMemoryAddress address, T value)
{
  using UnsignedType = typename std::make_unsigned_t<T>;
  static_assert(std::is_same_v<UnsignedType, u8> || std::is_same_v<UnsignedType, u16> ||
                std::is_same_v<UnsignedType, u32>);

  if constexpr (std::is_same_v<UnsignedType, u8>)
    CPU::SafeWriteMemoryByte(address, value);
  else if constexpr (std::is_same_v<UnsignedType, u16>)
    CPU::SafeWriteMemoryHalfWord(address, value);
  else // if constexpr (std::is_same_v<UnsignedType, u32>)
    CPU::SafeWriteMemoryWord(address, value);
}

static u32 GetControllerButtonBits()
{
  static constexpr std::array<u16, 16> button_mapping = {{
    0x0100, // Select
    0x0200, // L3
    0x0400, // R3
    0x0800, // Start
    0x1000, // Up
    0x2000, // Right
    0x4000, // Down
    0x8000, // Left
    0x0001, // L2
    0x0002, // R2
    0x0004, // L1
    0x0008, // R1
    0x0010, // Triangle
    0x0020, // Circle
    0x0040, // Cross
    0x0080, // Square
  }};

  u32 bits = 0;
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = System::GetController(i);
    if (!controller)
      continue;

    bits |= controller->GetButtonStateBits();
  }

  u32 translated_bits = 0;
  for (u32 i = 0, bit = 1; i < static_cast<u32>(button_mapping.size()); i++, bit <<= 1)
  {
    if (bits & bit)
      translated_bits |= button_mapping[i];
  }

  return translated_bits;
}

static u32 GetControllerAnalogBits()
{
  // 0x010000 - Right Thumb Up
  // 0x020000 - Right Thumb Right
  // 0x040000 - Right Thumb Down
  // 0x080000 - Right Thumb Left
  // 0x100000 - Left Thumb Up
  // 0x200000 - Left Thumb Right
  // 0x400000 - Left Thumb Down
  // 0x800000 - Left Thumb Left

  u32 bits = 0;
  u8 l_ypos = 0;
  u8 l_xpos = 0;
  u8 r_ypos = 0;
  u8 r_xpos = 0;

  std::optional<u32> analog = 0;
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = System::GetController(i);
    if (!controller)
      continue;

    analog = controller->GetAnalogInputBytes();
    if (analog.has_value())
    {
      l_ypos = Truncate8((analog.value() & 0xFF000000u) >> 24);
      l_xpos = Truncate8((analog.value() & 0x00FF0000u) >> 16);
      r_ypos = Truncate8((analog.value() & 0x0000FF00u) >> 8);
      r_xpos = Truncate8(analog.value() & 0x000000FFu);
      if (l_ypos < 0x50)
        bits |= 0x100000;
      else if (l_ypos > 0xA0)
        bits |= 0x400000;
      if (l_xpos < 0x50)
        bits |= 0x800000;
      else if (l_xpos > 0xA0)
        bits |= 0x200000;
      if (r_ypos < 0x50)
        bits |= 0x10000;
      else if (r_ypos > 0xA0)
        bits |= 0x40000;
      if (r_xpos < 0x50)
        bits |= 0x80000;
      else if (r_xpos > 0xA0)
        bits |= 0x20000;
    }
  }
  return bits;
}

CheatList::CheatList() = default;

CheatList::~CheatList() = default;

static bool IsHexCharacter(char c)
{
  return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
}

static int SignedCharToInt(char ch)
{
  return static_cast<int>(static_cast<unsigned char>(ch));
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
  std::optional<std::string> str = FileSystem::ReadFileToString(filename);
  if (!str.has_value() || str->empty())
    return false;

  return LoadFromPCSXRString(str.value());
}

bool CheatList::LoadFromPCSXRString(const std::string& str)
{
  std::istringstream iss(str);

  std::string line;
  std::string comments;
  std::string group;
  CheatCode::Type type = CheatCode::Type::Gameshark;
  CheatCode::Activation activation = CheatCode::Activation::EndFrame;
  CheatCode current_code;
  while (std::getline(iss, line))
  {
    char* start = line.data();
    while (*start != '\0' && std::isspace(SignedCharToInt(*start)))
      start++;

    // skip empty lines
    if (*start == '\0')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(SignedCharToInt(*end)))
    {
      *end = '\0';
      end--;
    }

    // DuckStation metadata
    if (StringUtil::Strncasecmp(start, "#group=", 7) == 0)
    {
      group = start + 7;
      continue;
    }
    if (StringUtil::Strncasecmp(start, "#type=", 6) == 0)
    {
      type = CheatCode::ParseTypeName(start + 6).value_or(CheatCode::Type::Gameshark);
      continue;
    }
    if (StringUtil::Strncasecmp(start, "#activation=", 12) == 0)
    {
      activation = CheatCode::ParseActivationName(start + 12).value_or(CheatCode::Activation::EndFrame);
      continue;
    }

    // skip comments and empty line
    if (*start == '#' || *start == ';' || *start == '/' || *start == '\"')
    {
      comments.append(start);
      comments += '\n';
      continue;
    }

    if (*start == '[' && *end == ']')
    {
      start++;
      *end = '\0';

      // new cheat
      if (current_code.Valid())
        m_codes.push_back(std::move(current_code));

      current_code = CheatCode();
      if (group.empty())
        group = "Ungrouped";

      current_code.group = std::move(group);
      group = std::string();
      current_code.comments = std::move(comments);
      comments = std::string();
      current_code.type = type;
      type = CheatCode::Type::Gameshark;
      current_code.activation = activation;
      activation = CheatCode::Activation::EndFrame;

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
  {
    // technically this isn't the place for end of file
    if (!comments.empty())
      current_code.comments += comments;
    m_codes.push_back(std::move(current_code));
  }

  Log_InfoPrintf("Loaded %zu cheats (PCSXR format)", m_codes.size());
  return !m_codes.empty();
}

bool CheatList::LoadFromLibretroFile(const char* filename)
{
  std::optional<std::string> str = FileSystem::ReadFileToString(filename);
  if (!str.has_value() || str->empty())
    return false;

  return LoadFromLibretroString(str.value());
}

bool CheatList::LoadFromLibretroString(const std::string& str)
{
  std::istringstream iss(str);
  std::string line;
  KeyValuePairVector kvp;
  while (std::getline(iss, line))
  {
    char* start = line.data();
    while (*start != '\0' && std::isspace(SignedCharToInt(*start)))
      start++;

    // skip empty lines
    if (*start == '\0' || *start == '=')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(SignedCharToInt(*end)))
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
    while (key_end > start && std::isspace(SignedCharToInt(*key_end)))
    {
      *key_end = '\0';
      key_end--;
    }

    char* value_start = equals + 1;
    while (*value_start != '\0' && std::isspace(SignedCharToInt(*value_start)))
      value_start++;

    if (*value_start == '\0')
      continue;

    char* value_end = value_start + std::strlen(value_start) - 1;
    while (value_end > value_start && std::isspace(SignedCharToInt(*value_end)))
    {
      *value_end = '\0';
      value_end--;
    }

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
  const u32 num_cheats = num_cheats_value ? StringUtil::FromChars<u32>(*num_cheats_value).value_or(0) : 0;
  if (num_cheats == 0)
    return false;

  for (u32 i = 0; i < num_cheats; i++)
  {
    const std::string* desc = FindKey(kvp, TinyString::FromFormat("cheat%u_desc", i));
    const std::string* code = FindKey(kvp, TinyString::FromFormat("cheat%u_code", i));
    const std::string* enable = FindKey(kvp, TinyString::FromFormat("cheat%u_enable", i));
    if (!desc || !code || !enable)
    {
      Log_WarningPrintf("Missing desc/code/enable for cheat %u", i);
      continue;
    }

    CheatCode cc;
    cc.group = "Ungrouped";
    cc.description = *desc;
    cc.enabled = StringUtil::FromChars<bool>(*enable).value_or(false);
    if (ParseLibretroCheat(&cc, code->c_str()))
      m_codes.push_back(std::move(cc));
  }

  Log_InfoPrintf("Loaded %zu cheats (libretro format)", m_codes.size());
  return !m_codes.empty();
}

bool CheatList::LoadFromEPSXeString(const std::string& str)
{
  std::istringstream iss(str);

  std::string line;
  std::string group;
  CheatCode::Type type = CheatCode::Type::Gameshark;
  CheatCode::Activation activation = CheatCode::Activation::EndFrame;
  CheatCode current_code;
  while (std::getline(iss, line))
  {
    char* start = line.data();
    while (*start != '\0' && std::isspace(SignedCharToInt(*start)))
      start++;

    // skip empty lines
    if (*start == '\0')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(SignedCharToInt(*end)))
    {
      *end = '\0';
      end--;
    }

    // skip comments and empty line
    if (*start == ';' || *start == '\0')
      continue;

    if (*start == '#')
    {
      start++;

      // new cheat
      if (current_code.Valid())
        m_codes.push_back(std::move(current_code));

      current_code = CheatCode();
      if (group.empty())
        group = "Ungrouped";

      current_code.group = std::move(group);
      group = std::string();
      current_code.type = type;
      type = CheatCode::Type::Gameshark;
      current_code.activation = activation;
      activation = CheatCode::Activation::EndFrame;

      char* separator = std::strchr(start, '\\');
      if (separator)
      {
        *separator = 0;
        current_code.group = start;
        start = separator + 1;
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

  Log_InfoPrintf("Loaded %zu cheats (EPSXe format)", m_codes.size());
  return !m_codes.empty();
}

static bool IsLibretroSeparator(char ch)
{
  return (ch == ' ' || ch == '-' || ch == ':' || ch == '+');
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
      if (!IsLibretroSeparator(*end_ptr))
      {
        Log_WarningPrintf("Malformed code '%s'", line);
        break;
      }

      end_ptr++;
      inst.second = static_cast<u32>(std::strtoul(current_ptr, &end_ptr, 16));
      if (end_ptr && *end_ptr == '\0')
        end_ptr = nullptr;

      if (end_ptr && *end_ptr != '\0')
      {
        if (!IsLibretroSeparator(*end_ptr))
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
  if (!m_master_enable)
    return;

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
  std::optional<std::string> str = FileSystem::ReadFileToString(filename);
  if (!str.has_value() || str->empty())
    return std::nullopt;

  return DetectFileFormat(str.value());
}

CheatList::Format CheatList::DetectFileFormat(const std::string& str)
{
  std::istringstream iss(str);
  std::string line;
  while (std::getline(iss, line))
  {
    char* start = line.data();
    while (*start != '\0' && std::isspace(SignedCharToInt(*start)))
      start++;

    // skip empty lines
    if (*start == '\0')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(SignedCharToInt(*end)))
    {
      *end = '\0';
      end--;
    }

    // eat comments
    if (start[0] == '#' || start[0] == ';')
      continue;

    if (std::strncmp(line.data(), "cheats", 6) == 0)
      return Format::Libretro;

    // pcsxr if we see brackets
    if (start[0] == '[')
      return Format::PCSXR;

    // otherwise if it's a code, it's probably epsxe
    if (std::isdigit(start[0]))
      return Format::EPSXe;
  }

  return Format::Count;
}

bool CheatList::LoadFromFile(const char* filename, Format format)
{
  if (!FileSystem::FileExists(filename))
    return false;

  std::optional<std::string> str = FileSystem::ReadFileToString(filename);
  if (!str.has_value())
    return false;

  if (str->empty())
    return true;

  return LoadFromString(str.value(), format);
}

bool CheatList::LoadFromString(const std::string& str, Format format)
{
  if (format == Format::Autodetect)
    format = DetectFileFormat(str);

  if (format == Format::PCSXR)
    return LoadFromPCSXRString(str);
  else if (format == Format::Libretro)
    return LoadFromLibretroString(str);
  format = Format::EPSXe;
  return LoadFromEPSXeString(str);
}

bool CheatList::SaveToPCSXRFile(const char* filename)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "wb");
  if (!fp)
    return false;

  for (const CheatCode& cc : m_codes)
  {
    if (!cc.comments.empty())
      std::fputs(cc.comments.c_str(), fp.get());
    std::fprintf(fp.get(), "#group=%s\n", cc.group.c_str());
    std::fprintf(fp.get(), "#type=%s\n", CheatCode::GetTypeName(cc.type));
    std::fprintf(fp.get(), "#activation=%s\n", CheatCode::GetActivationName(cc.activation));
    std::fprintf(fp.get(), "[%s%s]\n", cc.enabled ? "*" : "", cc.description.c_str());
    for (const CheatCode::Instruction& i : cc.instructions)
      std::fprintf(fp.get(), "%08X %04X\n", i.first, i.second);
    std::fprintf(fp.get(), "\n");
  }

  std::fflush(fp.get());
  return (std::ferror(fp.get()) == 0);
}

bool CheatList::LoadFromPackage(const std::string& game_code)
{
  std::unique_ptr<ByteStream> stream =
    g_host_interface->OpenPackageFile("database/chtdb.txt", BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return false;

  std::string db_string = FileSystem::ReadStreamToString(stream.get());
  stream.reset();
  if (db_string.empty())
    return false;

  std::istringstream iss(db_string);
  std::string line;
  while (std::getline(iss, line))
  {
    char* start = line.data();
    while (*start != '\0' && std::isspace(SignedCharToInt(*start)))
      start++;

    // skip empty lines
    if (*start == '\0' || *start == ';')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(SignedCharToInt(*end)))
    {
      *end = '\0';
      end--;
    }

    if (start == end)
      continue;

    if (start[0] != ':' || std::strcmp(&start[1], game_code.c_str()) != 0)
      continue;

    // game code match
    CheatCode current_code;
    while (std::getline(iss, line))
    {
      start = line.data();
      while (*start != '\0' && std::isspace(SignedCharToInt(*start)))
        start++;

      // skip empty lines
      if (*start == '\0' || *start == ';')
        continue;

      end = start + std::strlen(start) - 1;
      while (end > start && std::isspace(SignedCharToInt(*end)))
      {
        *end = '\0';
        end--;
      }

      if (start == end)
        continue;

      // stop adding codes when we hit a different game
      if (start[0] == ':' && (!m_codes.empty() || current_code.Valid()))
        break;

      if (start[0] == '#')
      {
        start++;

        if (current_code.Valid())
        {
          m_codes.push_back(std::move(current_code));
          current_code = CheatCode();
        }

        // new code
        char* slash = std::strrchr(start, '\\');
        if (slash)
        {
          *slash = '\0';
          current_code.group = start;
          start = slash + 1;
        }
        if (current_code.group.empty())
          current_code.group = "Ungrouped";

        current_code.description = start;
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

    Log_InfoPrintf("Loaded %zu codes from package for %s", m_codes.size(), game_code.c_str());
    return !m_codes.empty();
  }

  Log_WarningPrintf("No codes found in package for %s", game_code.c_str());
  return false;
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

std::vector<std::string> CheatList::GetCodeGroups() const
{
  std::vector<std::string> groups;
  for (const CheatCode& cc : m_codes)
  {
    if (std::any_of(groups.begin(), groups.end(), [cc](const std::string& group) { return (group == cc.group); }))
      continue;

    groups.emplace_back(cc.group);
  }

  return groups;
}

void CheatList::SetCodeEnabled(u32 index, bool state)
{
  if (index >= m_codes.size() || m_codes[index].enabled == state)
    return;

  m_codes[index].enabled = state;
  if (!state)
    m_codes[index].ApplyOnDisable();
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

const CheatCode* CheatList::FindCode(const char* name) const
{
  for (const CheatCode& cc : m_codes)
  {
    if (cc.description == name)
      return &cc;
  }

  return nullptr;
}

const CheatCode* CheatList::FindCode(const char* group, const char* name) const
{
  for (const CheatCode& cc : m_codes)
  {
    if (cc.group == group && cc.description == name)
      return &cc;
  }

  return nullptr;
}

void CheatList::MergeList(const CheatList& cl)
{
  for (const CheatCode& cc : cl.m_codes)
  {
    if (!FindCode(cc.group.c_str(), cc.description.c_str()))
      AddCode(cc);
  }
}

std::string CheatCode::GetInstructionsAsString() const
{
  std::stringstream ss;

  for (const Instruction& inst : instructions)
  {
    ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << inst.first;
    ss << " ";
    ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << inst.second;
    ss << '\n';
  }

  return ss.str();
}

bool CheatCode::SetInstructionsFromString(const std::string& str)
{
  std::vector<Instruction> new_instructions;
  std::istringstream ss(str);

  for (std::string line; std::getline(ss, line);)
  {
    char* start = line.data();
    while (*start != '\0' && std::isspace(SignedCharToInt(*start)))
      start++;

    // skip empty lines
    if (*start == '\0')
      continue;

    char* end = start + std::strlen(start) - 1;
    while (end > start && std::isspace(SignedCharToInt(*end)))
    {
      *end = '\0';
      end--;
    }

    // skip comments and empty line
    if (*start == '#' || *start == ';' || *start == '/' || *start == '\"')
      continue;

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
    new_instructions.push_back(inst);
  }

  if (new_instructions.empty())
    return false;

  instructions = std::move(new_instructions);
  return true;
}

static bool IsConditionalInstruction(CheatCode::InstructionCode code)
{
  switch (code)
  {
    case CheatCode::InstructionCode::CompareEqual16:       // D0
    case CheatCode::InstructionCode::CompareNotEqual16:    // D1
    case CheatCode::InstructionCode::CompareLess16:        // D2
    case CheatCode::InstructionCode::CompareGreater16:     // D3
    case CheatCode::InstructionCode::CompareEqual8:        // E0
    case CheatCode::InstructionCode::CompareNotEqual8:     // E1
    case CheatCode::InstructionCode::CompareLess8:         // E2
    case CheatCode::InstructionCode::CompareGreater8:      // E3
    case CheatCode::InstructionCode::CompareButtons:       // D4
    case CheatCode::InstructionCode::ExtCompareEqual32:    // A0
    case CheatCode::InstructionCode::ExtCompareNotEqual32: // A1
    case CheatCode::InstructionCode::ExtCompareLess32:     // A2
    case CheatCode::InstructionCode::ExtCompareGreater32:  // A3
      return true;

    default:
      return false;
  }
}

u32 CheatCode::GetNextNonConditionalInstruction(u32 index) const
{
  const u32 count = static_cast<u32>(instructions.size());
  for (; index < count; index++)
  {
    if (!IsConditionalInstruction(instructions[index].code))
    {
      // we've found the first non conditional instruction in the chain, so skip over the instruction following it
      return index + 1;
    }
  }

  return index;
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
      case InstructionCode::Nop:
      {
        index++;
      }
      break;

      case InstructionCode::ConstantWrite8:
      {
        DoMemoryWrite<u8>(inst.address, inst.value8);
        index++;
      }
      break;

      case InstructionCode::ConstantWrite16:
      {
        DoMemoryWrite<u16>(inst.address, inst.value16);
        index++;
      }
      break;

      case InstructionCode::ExtConstantWrite32:
      {
        DoMemoryWrite<u32>(inst.address, inst.value32);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitSet8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address) | inst.value8;
        DoMemoryWrite<u8>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitSet16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address) | inst.value16;
        DoMemoryWrite<u16>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitSet32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address) | inst.value32;
        DoMemoryWrite<u32>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitClear8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address) & ~inst.value8;
        DoMemoryWrite<u8>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitClear16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address) & ~inst.value16;
        DoMemoryWrite<u16>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ExtConstantBitClear32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address) & ~inst.value32;
        DoMemoryWrite<u32>(inst.address, value);
        index++;
      }
      break;

      case InstructionCode::ScratchpadWrite16:
      {
        DoMemoryWrite<u16>(CPU::DCACHE_LOCATION | (inst.address & CPU::DCACHE_OFFSET_MASK), inst.value16);
        index++;
      }
      break;

      case InstructionCode::ExtScratchpadWrite32:
      {
        DoMemoryWrite<u32>(CPU::DCACHE_LOCATION | (inst.address & CPU::DCACHE_OFFSET_MASK), inst.value32);
        index++;
      }
      break;

      case InstructionCode::ExtIncrement32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        DoMemoryWrite<u32>(inst.address, value + inst.value32);
        index++;
      }
      break;

      case InstructionCode::ExtDecrement32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        DoMemoryWrite<u32>(inst.address, value - inst.value32);
        index++;
      }
      break;

      case InstructionCode::Increment16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        DoMemoryWrite<u16>(inst.address, value + inst.value16);
        index++;
      }
      break;

      case InstructionCode::Decrement16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        DoMemoryWrite<u16>(inst.address, value - inst.value16);
        index++;
      }
      break;

      case InstructionCode::Increment8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        DoMemoryWrite<u8>(inst.address, value + inst.value8);
        index++;
      }
      break;

      case InstructionCode::Decrement8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        DoMemoryWrite<u8>(inst.address, value - inst.value8);
        index++;
      }
      break;

      case InstructionCode::ExtCompareEqual32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        if (value == inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCompareNotEqual32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        if (value != inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCompareLess32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        if (value < inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCompareGreater32:
      {
        const u32 value = DoMemoryRead<u32>(inst.address);
        if (value > inst.value32)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtConstantWriteIfMatch16:
      case InstructionCode::ExtConstantWriteIfMatchWithRestore16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        const u16 comparevalue = Truncate16(inst.value32 >> 16);
        const u16 newvalue = Truncate16(inst.value32 & 0xFFFFu);
        if (value == comparevalue)
          DoMemoryWrite<u16>(inst.address, newvalue);

        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRange8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        const u8 min = Truncate8(inst.value32 & 0x000000FFu);
        const u8 max = Truncate8((inst.value32 & 0x0000FF00u) >> 8);
        const u8 overmin = Truncate8((inst.value32 & 0x00FF0000u) >> 16);
        const u8 overmax = Truncate8((inst.value32 & 0xFF000000u) >> 24);
        if ((value < min) || (value < min && min == 0x00u && max < 0xFEu))
          DoMemoryWrite<u8>(inst.address, overmin); // also handles a min value of 0x00
        else if (value > max)
          DoMemoryWrite<u8>(inst.address, overmax);
        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRangeLimits16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        const u16 min = Truncate16(inst.value32 & 0x0000FFFFu);
        const u16 max = Truncate16((inst.value32 & 0xFFFF0000u) >> 16);
        if ((value < min) || (value < min && min == 0x0000u && max < 0xFFFEu))
          DoMemoryWrite<u16>(inst.address, min); // also handles a min value of 0x0000
        else if (value > max)
          DoMemoryWrite<u16>(inst.address, max);
        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRangeRollRound16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        const u16 min = Truncate16(inst.value32 & 0x0000FFFFu);
        const u16 max = Truncate16((inst.value32 & 0xFFFF0000u) >> 16);
        if ((value < min) || (value < min && min == 0x0000u && max < 0xFFFEu))
          DoMemoryWrite<u16>(inst.address, max); // also handles a min value of 0x0000
        else if (value > max)
          DoMemoryWrite<u16>(inst.address, min);
        index++;
      }
      break;

      case InstructionCode::ExtConstantForceRange16:
      {
        const u16 min = Truncate16(inst.value32 & 0x0000FFFFu);
        const u16 max = Truncate16((inst.value32 & 0xFFFF0000u) >> 16);
        const u16 value = DoMemoryRead<u16>(inst.address);
        const Instruction& inst2 = instructions[index + 1];
        const u16 overmin = Truncate16(inst2.value32 & 0x0000FFFFu);
        const u16 overmax = Truncate16((inst2.value32 & 0xFFFF0000u) >> 16);

        if ((value < min) || (value < min && min == 0x0000u && max < 0xFFFEu))
          DoMemoryWrite<u16>(inst.address, overmin); // also handles a min value of 0x0000
        else if (value > max)
          DoMemoryWrite<u16>(inst.address, overmax);
        index += 2;
      }
      break;

      case InstructionCode::ExtConstantSwap16:
      {
        const u16 value1 = Truncate16(inst.value32 & 0x0000FFFFu);
        const u16 value2 = Truncate16((inst.value32 & 0xFFFF0000u) >> 16);
        const u16 value = DoMemoryRead<u16>(inst.address);

        if (value == value1)
          DoMemoryWrite<u16>(inst.address, value2);
        else if (value == value2)
          DoMemoryWrite<u16>(inst.address, value1);
        index++;
      }
      break;

      case InstructionCode::ExtFindAndReplace:
      {

        if ((index + 4) >= instructions.size())
        {
          Log_ErrorPrintf("Incomplete find/replace instruction");
          return;
        }
        const Instruction& inst2 = instructions[index + 1];
        const Instruction& inst3 = instructions[index + 2];
        const Instruction& inst4 = instructions[index + 3];
        const Instruction& inst5 = instructions[index + 4];

        const u32 offset = Truncate16(inst.value32 & 0x0000FFFFu) << 1;
        const u8 wildcard = Truncate8((inst.value32 & 0x00FF0000u) >> 16);
        const u32 minaddress = inst.address - offset;
        const u32 maxaddress = inst.address + offset;
        const u8 f1 = Truncate8((inst2.first & 0xFF000000u) >> 24);
        const u8 f2 = Truncate8((inst2.first & 0x00FF0000u) >> 16);
        const u8 f3 = Truncate8((inst2.first & 0x0000FF00u) >> 8);
        const u8 f4 = Truncate8(inst2.first & 0x000000FFu);
        const u8 f5 = Truncate8((inst2.value32 & 0xFF000000u) >> 24);
        const u8 f6 = Truncate8((inst2.value32 & 0x00FF0000u) >> 16);
        const u8 f7 = Truncate8((inst2.value32 & 0x0000FF00u) >> 8);
        const u8 f8 = Truncate8(inst2.value32 & 0x000000FFu);
        const u8 f9 = Truncate8((inst3.first & 0xFF000000u) >> 24);
        const u8 f10 = Truncate8((inst3.first & 0x00FF0000u) >> 16);
        const u8 f11 = Truncate8((inst3.first & 0x0000FF00u) >> 8);
        const u8 f12 = Truncate8(inst3.first & 0x000000FFu);
        const u8 f13 = Truncate8((inst3.value32 & 0xFF000000u) >> 24);
        const u8 f14 = Truncate8((inst3.value32 & 0x00FF0000u) >> 16);
        const u8 f15 = Truncate8((inst3.value32 & 0x0000FF00u) >> 8);
        const u8 f16 = Truncate8(inst3.value32 & 0x000000FFu);
        const u8 r1 = Truncate8((inst4.first & 0xFF000000u) >> 24);
        const u8 r2 = Truncate8((inst4.first & 0x00FF0000u) >> 16);
        const u8 r3 = Truncate8((inst4.first & 0x0000FF00u) >> 8);
        const u8 r4 = Truncate8(inst4.first & 0x000000FFu);
        const u8 r5 = Truncate8((inst4.value32 & 0xFF000000u) >> 24);
        const u8 r6 = Truncate8((inst4.value32 & 0x00FF0000u) >> 16);
        const u8 r7 = Truncate8((inst4.value32 & 0x0000FF00u) >> 8);
        const u8 r8 = Truncate8(inst4.value32 & 0x000000FFu);
        const u8 r9 = Truncate8((inst5.first & 0xFF000000u) >> 24);
        const u8 r10 = Truncate8((inst5.first & 0x00FF0000u) >> 16);
        const u8 r11 = Truncate8((inst5.first & 0x0000FF00u) >> 8);
        const u8 r12 = Truncate8(inst5.first & 0x000000FFu);
        const u8 r13 = Truncate8((inst5.value32 & 0xFF000000u) >> 24);
        const u8 r14 = Truncate8((inst5.value32 & 0x00FF0000u) >> 16);
        const u8 r15 = Truncate8((inst5.value32 & 0x0000FF00u) >> 8);
        const u8 r16 = Truncate8(inst5.value32 & 0x000000FFu);

        for (u32 address = minaddress; address <= maxaddress; address += 2)
        {
          if ((DoMemoryRead<u8>(address) == f1 || f1 == wildcard) &&
              (DoMemoryRead<u8>(address + 1) == f2 || f2 == wildcard) &&
              (DoMemoryRead<u8>(address + 2) == f3 || f3 == wildcard) &&
              (DoMemoryRead<u8>(address + 3) == f4 || f4 == wildcard) &&
              (DoMemoryRead<u8>(address + 4) == f5 || f5 == wildcard) &&
              (DoMemoryRead<u8>(address + 5) == f6 || f6 == wildcard) &&
              (DoMemoryRead<u8>(address + 6) == f7 || f7 == wildcard) &&
              (DoMemoryRead<u8>(address + 7) == f8 || f8 == wildcard) &&
              (DoMemoryRead<u8>(address + 8) == f9 || f9 == wildcard) &&
              (DoMemoryRead<u8>(address + 9) == f10 || f10 == wildcard) &&
              (DoMemoryRead<u8>(address + 10) == f11 || f11 == wildcard) &&
              (DoMemoryRead<u8>(address + 11) == f12 || f12 == wildcard) &&
              (DoMemoryRead<u8>(address + 12) == f13 || f13 == wildcard) &&
              (DoMemoryRead<u8>(address + 13) == f14 || f14 == wildcard) &&
              (DoMemoryRead<u8>(address + 14) == f15 || f15 == wildcard) &&
              (DoMemoryRead<u8>(address + 15) == f16 || f16 == wildcard))
          {
            if (r1 != wildcard)
              DoMemoryWrite<u8>(address, r1);
            if (r2 != wildcard)
              DoMemoryWrite<u8>(address + 1, r2);
            if (r3 != wildcard)
              DoMemoryWrite<u8>(address + 2, r3);
            if (r4 != wildcard)
              DoMemoryWrite<u8>(address + 3, r4);
            if (r5 != wildcard)
              DoMemoryWrite<u8>(address + 4, r5);
            if (r6 != wildcard)
              DoMemoryWrite<u8>(address + 5, r6);
            if (r7 != wildcard)
              DoMemoryWrite<u8>(address + 6, r7);
            if (r8 != wildcard)
              DoMemoryWrite<u8>(address + 7, r8);
            if (r9 != wildcard)
              DoMemoryWrite<u8>(address + 8, r9);
            if (r10 != wildcard)
              DoMemoryWrite<u8>(address + 9, r10);
            if (r11 != wildcard)
              DoMemoryWrite<u8>(address + 10, r11);
            if (r12 != wildcard)
              DoMemoryWrite<u8>(address + 11, r12);
            if (r13 != wildcard)
              DoMemoryWrite<u8>(address + 12, r13);
            if (r14 != wildcard)
              DoMemoryWrite<u8>(address + 13, r14);
            if (r15 != wildcard)
              DoMemoryWrite<u8>(address + 14, r15);
            if (r16 != wildcard)
              DoMemoryWrite<u8>(address + 15, r16);
            address = address + 15;
          }
        }
        index += 5;
      }
      break;

      case InstructionCode::CompareEqual16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        if (value == inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareNotEqual16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        if (value != inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareLess16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        if (value < inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareGreater16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        if (value > inst.value16)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareEqual8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        if (value == inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareNotEqual8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        if (value != inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareLess8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        if (value < inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareGreater8:
      {
        const u8 value = DoMemoryRead<u8>(inst.address);
        if (value > inst.value8)
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::CompareButtons: // D4
      {
        if (inst.value16 == GetControllerButtonBits())
          index++;
        else
          index = GetNextNonConditionalInstruction(index);
      }
      break;

      case InstructionCode::ExtCheatRegisters: // 51
      {
        const u32 poke_value = inst.value32;
        const u8 cht_reg_no1 = Truncate8(inst.address & 0xFFu);
        const u8 cht_reg_no2 = Truncate8((inst.address & 0xFF00u) >> 8);
        const u8 cht_reg_no3 = Truncate8(inst.value32 & 0xFFu);
        const u8 sub_type = Truncate8((inst.address & 0xFF0000u) >> 16);

        switch (sub_type)
        {
          case 0x00: // Write the u8 from cht_register[cht_reg_no1] to address
            DoMemoryWrite<u8>(inst.value32, Truncate8(cht_register[cht_reg_no1]) & 0xFFu);
            break;
          case 0x01: // Read the u8 from address to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<u8>(inst.value32);
            break;
          case 0x02: // Write the u8 from address field to the address stored in cht_register[cht_reg_no1]
            DoMemoryWrite<u8>(cht_register[cht_reg_no1], Truncate8(poke_value & 0xFFu));
            break;
          case 0x03: // Write the u8 from cht_register[cht_reg_no2] to cht_register[cht_reg_no1]
                     // and add the u8 from the address field to it
            cht_register[cht_reg_no1] = Truncate8(cht_register[cht_reg_no2] & 0xFFu) + Truncate8(poke_value & 0xFFu);
            break;
          case 0x04: // Write the u8 from the value stored in cht_register[cht_reg_no2] + poke_value to the address
                     // stored in cht_register[cht_reg_no1]
            DoMemoryWrite<u8>(cht_register[cht_reg_no1],
                              Truncate8(cht_register[cht_reg_no2] & 0xFFu) + Truncate8(poke_value & 0xFFu));
            break;
          case 0x05: // Write the u8 poke value to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = Truncate8(poke_value & 0xFFu);
            break;

          case 0x40: // Write the u16 from cht_register[cht_reg_no1] to address
            DoMemoryWrite<u16>(inst.value32, Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x41: // Read the u16 from address to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<u16>(inst.value32);
            break;
          case 0x42: // Write the u16 from address field to the address stored in cht_register[cht_reg_no1]
            DoMemoryWrite<u16>(cht_register[cht_reg_no1], Truncate16(poke_value & 0xFFFFu));
            break;
          case 0x43: // Write the u16 from cht_register[cht_reg_no2] to cht_register[cht_reg_no1]
                     // and add the u16 from the address field to it
            cht_register[cht_reg_no1] =
              Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) + Truncate16(poke_value & 0xFFFFu);
            break;
          case 0x44: // Write the u16 from the value stored in cht_register[cht_reg_no2] + poke_value to the address
                     // stored in cht_register[cht_reg_no1]
            DoMemoryWrite<u16>(cht_register[cht_reg_no1],
                               Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) + Truncate16(poke_value & 0xFFFFu));
            break;
          case 0x45: // Write the u16 poke value to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = Truncate16(poke_value & 0xFFFFu);
            break;

          case 0x80: // Write the u32 from cht_register[cht_reg_no1] to address
            DoMemoryWrite<u32>(inst.value32, cht_register[cht_reg_no1]);
            break;
          case 0x81: // Read the u32 from address to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = DoMemoryRead<u32>(inst.value32);
            break;
          case 0x82: // Write the u32 from address field to the address stored in cht_register[cht_reg_no]
            DoMemoryWrite<u32>(cht_register[cht_reg_no1], poke_value);
            break;
          case 0x83: // Write the u32 from cht_register[cht_reg_no2] to cht_register[cht_reg_no1]
                     // and add the u32 from the address field to it
            cht_register[cht_reg_no1] = cht_register[cht_reg_no2] + poke_value;
            break;
          case 0x84: // Write the u32 from the value stored in cht_register[cht_reg_no2] + poke_value to the address
                     // stored in cht_register[cht_reg_no1]
            DoMemoryWrite<u32>(cht_register[cht_reg_no1], cht_register[cht_reg_no2] + poke_value);
            break;
          case 0x85: // Write the u32 poke value to cht_register[cht_reg_no1]
            cht_register[cht_reg_no1] = poke_value;
            break;

          case 0xC0: // Reg3 = Reg2 + Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] + cht_register[cht_reg_no1];
            break;
          case 0xC1: // Reg3 = Reg2 - Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] - cht_register[cht_reg_no1];
            break;
          case 0xC2: // Reg3 = Reg2 * Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] * cht_register[cht_reg_no1];
            break;
          case 0xC3: // Reg3 = Reg2 / Reg1 with DIV0 handling
            if (cht_register[cht_reg_no1] == 0)
              cht_register[cht_reg_no3] = 0;
            else
              cht_register[cht_reg_no3] = cht_register[cht_reg_no2] / cht_register[cht_reg_no1];
            break;
          case 0xC4: // Reg3 = Reg2 % Reg1 (with DIV0 handling)
            if (cht_register[cht_reg_no1] == 0)
              cht_register[cht_reg_no3] = cht_register[cht_reg_no2];
            else
              cht_register[cht_reg_no3] = cht_register[cht_reg_no2] % cht_register[cht_reg_no1];
            break;
          case 0xC5: // Reg3 = Reg2 & Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] & cht_register[cht_reg_no1];
            break;
          case 0xC6: // Reg3 = Reg2 | Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] | cht_register[cht_reg_no1];
            break;
          case 0xC7: // Reg3 = Reg2 ^ Reg1
            cht_register[cht_reg_no3] = cht_register[cht_reg_no2] ^ cht_register[cht_reg_no1];
            break;
          case 0xC8: // Reg3 = ~Reg1
            cht_register[cht_reg_no3] = ~cht_register[cht_reg_no1];
            break;
          case 0xC9: // Reg3 = Reg1 << X
            cht_register[cht_reg_no3] = cht_register[cht_reg_no1] << cht_reg_no2;
            break;
          case 0xCA: // Reg3 = Reg1 >> X
            cht_register[cht_reg_no3] = cht_register[cht_reg_no1] >> cht_reg_no2;
            break;
          // Lots of options exist for expanding into this space
          default:
            break;
        }
        index++;
      }
      break;

      case InstructionCode::SkipIfNotEqual16:      // C0
      case InstructionCode::ExtSkipIfNotEqual32:   // A4
      case InstructionCode::SkipIfButtonsNotEqual: // D5
      case InstructionCode::SkipIfButtonsEqual:    // D6
      case InstructionCode::ExtSkipIfNotLess8:     // C3
      case InstructionCode::ExtSkipIfNotGreater8:  // C4
      case InstructionCode::ExtSkipIfNotLess16:    // C5
      case InstructionCode::ExtSkipIfNotGreater16: // C6
      case InstructionCode::ExtMultiConditionals:  // F6
      {
        index++;

        bool activate_codes;
        switch (inst.code)
        {
          case InstructionCode::SkipIfNotEqual16: // C0
            activate_codes = (DoMemoryRead<u16>(inst.address) == inst.value16);
            break;
          case InstructionCode::ExtSkipIfNotEqual32: // A4
            activate_codes = (DoMemoryRead<u32>(inst.address) == inst.value32);
            break;
          case InstructionCode::SkipIfButtonsNotEqual: // D5
            activate_codes = (GetControllerButtonBits() == inst.value16);
            break;
          case InstructionCode::SkipIfButtonsEqual: // D6
            activate_codes = (GetControllerButtonBits() != inst.value16);
            break;
          case InstructionCode::ExtSkipIfNotLess8: // C3
            activate_codes = (DoMemoryRead<u8>(inst.address) < inst.value8);
            break;
          case InstructionCode::ExtSkipIfNotGreater8: // C4
            activate_codes = (DoMemoryRead<u8>(inst.address) > inst.value8);
            break;
          case InstructionCode::ExtSkipIfNotLess16: // C5
            activate_codes = (DoMemoryRead<u16>(inst.address) < inst.value16);
            break;
          case InstructionCode::ExtSkipIfNotGreater16: // C6
            activate_codes = (DoMemoryRead<u16>(inst.address) > inst.value16);
            break;
          case InstructionCode::ExtMultiConditionals: // F6
          {
            // Ensure any else if or else that are hit outside the if context are skipped
            if ((inst.value32 & 0xFFFFFF00u) != 0x1F000000)
            {
              activate_codes = false;
              break;
            }
            for (;;)
            {
              const u8 totalConds = Truncate8(instructions[index - 1].value32 & 0x000000FFu);
              const u8 conditionType = Truncate8(instructions[index - 1].address & 0x000000FFu);

              bool conditions_check;

              if (conditionType == 0x00 && totalConds > 0) // AND
              {
                conditions_check = true;

                for (int i = 1; totalConds >= i; index++, i++)
                {
                  switch (instructions[index].code)
                  {
                    case InstructionCode::CompareEqual16: // D0
                      conditions_check &=
                        (DoMemoryRead<u16>(instructions[index].address) == instructions[index].value16);
                      break;
                    case InstructionCode::CompareNotEqual16: // D1
                      conditions_check &=
                        (DoMemoryRead<u16>(instructions[index].address) != instructions[index].value16);
                      break;
                    case InstructionCode::CompareLess16: // D2
                      conditions_check &=
                        (DoMemoryRead<u16>(instructions[index].address) < instructions[index].value16);
                      break;
                    case InstructionCode::CompareGreater16: // D3
                      conditions_check &=
                        (DoMemoryRead<u16>(instructions[index].address) > instructions[index].value16);
                      break;
                    case InstructionCode::CompareEqual8: // E0
                      conditions_check &= (DoMemoryRead<u8>(instructions[index].address) == instructions[index].value8);
                      break;
                    case InstructionCode::CompareNotEqual8: // E1
                      conditions_check &= (DoMemoryRead<u8>(instructions[index].address) != instructions[index].value8);
                      break;
                    case InstructionCode::CompareLess8: // E2
                      conditions_check &= (DoMemoryRead<u8>(instructions[index].address) < instructions[index].value8);
                      break;
                    case InstructionCode::CompareGreater8: // E3
                      conditions_check &= (DoMemoryRead<u8>(instructions[index].address) > instructions[index].value8);
                      break;
                    case InstructionCode::ExtCompareEqual32: // A0
                      conditions_check &=
                        (DoMemoryRead<u32>(instructions[index].address) == instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareNotEqual32: // A1
                      conditions_check &=
                        (DoMemoryRead<u32>(instructions[index].address) != instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareLess32: // A2
                      conditions_check &=
                        (DoMemoryRead<u32>(instructions[index].address) < instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareGreater32: // A3
                      conditions_check &=
                        (DoMemoryRead<u32>(instructions[index].address) > instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareBitsSet8: // E4 Internal to F6
                      conditions_check &=
                        (instructions[index].value8 ==
                         (DoMemoryRead<u8>(instructions[index].address) & instructions[index].value8));
                      break;
                    case InstructionCode::ExtCompareBitsClear8: // E5 Internal to F6
                      conditions_check &=
                        ((DoMemoryRead<u8>(instructions[index].address) & instructions[index].value8) == 0);
                      break;
                    case InstructionCode::ExtBitCompareButtons: // D7
                    {
                      const u32 frame_compare_value = instructions[index].address & 0xFFFFu;
                      const u8 cht_reg_no = Truncate8((instructions[index].value32 & 0xFF000000u) >> 24);
                      const bool bit_comparison_type = ((instructions[index].address & 0x100000u) >> 20);
                      const u8 frame_comparison = Truncate8((instructions[index].address & 0xF0000u) >> 16);
                      const u32 check_value = (instructions[index].value32 & 0xFFFFFFu);
                      const u32 value1 = GetControllerButtonBits();
                      const u32 value2 = GetControllerAnalogBits();
                      u32 value = value1 | value2;

                      if ((bit_comparison_type == false && check_value == (value & check_value)) // Check Bits are set
                          ||
                          (bit_comparison_type == true && check_value != (value & check_value))) // Check Bits are clear
                      {
                        cht_register[cht_reg_no] += 1;
                        switch (frame_comparison)
                        {
                          case 0x0: // No comparison on frame count, just do it
                            conditions_check &= true;
                            break;
                          case 0x1: // Check if frame_compare_value == current count
                            conditions_check &= (cht_register[cht_reg_no] == frame_compare_value);
                            break;
                          case 0x2: // Check if frame_compare_value < current count
                            conditions_check &= (cht_register[cht_reg_no] < frame_compare_value);
                            break;
                          case 0x3: // Check if frame_compare_value > current count
                            conditions_check &= (cht_register[cht_reg_no] > frame_compare_value);
                            break;
                          case 0x4: // Check if frame_compare_value != current count
                            conditions_check &= (cht_register[cht_reg_no] != frame_compare_value);
                            break;
                          default:
                            conditions_check &= false;
                            break;
                        }
                      }
                      else
                      {
                        cht_register[cht_reg_no] = 0;
                        conditions_check &= false;
                      }
                      break;
                    }
                    default:
                      Log_ErrorPrintf("Incorrect conditional instruction (see chtdb.txt for supported instructions)");
                      return;
                  }
                }
              }
              else if (conditionType == 0x01 && totalConds > 0) // OR
              {
                conditions_check = false;

                for (int i = 1; totalConds >= i; index++, i++)
                {
                  switch (instructions[index].code)
                  {
                    case InstructionCode::CompareEqual16: // D0
                      conditions_check |=
                        (DoMemoryRead<u16>(instructions[index].address) == instructions[index].value16);
                      break;
                    case InstructionCode::CompareNotEqual16: // D1
                      conditions_check |=
                        (DoMemoryRead<u16>(instructions[index].address) != instructions[index].value16);
                      break;
                    case InstructionCode::CompareLess16: // D2
                      conditions_check |=
                        (DoMemoryRead<u16>(instructions[index].address) < instructions[index].value16);
                      break;
                    case InstructionCode::CompareGreater16: // D3
                      conditions_check |=
                        (DoMemoryRead<u16>(instructions[index].address) > instructions[index].value16);
                      break;
                    case InstructionCode::CompareEqual8: // E0
                      conditions_check |= (DoMemoryRead<u8>(instructions[index].address) == instructions[index].value8);
                      break;
                    case InstructionCode::CompareNotEqual8: // E1
                      conditions_check |= (DoMemoryRead<u8>(instructions[index].address) != instructions[index].value8);
                      break;
                    case InstructionCode::CompareLess8: // E2
                      conditions_check |= (DoMemoryRead<u8>(instructions[index].address) < instructions[index].value8);
                      break;
                    case InstructionCode::CompareGreater8: // E3
                      conditions_check |= (DoMemoryRead<u8>(instructions[index].address) > instructions[index].value8);
                      break;
                    case InstructionCode::ExtCompareEqual32: // A0
                      conditions_check |=
                        (DoMemoryRead<u32>(instructions[index].address) == instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareNotEqual32: // A1
                      conditions_check |=
                        (DoMemoryRead<u32>(instructions[index].address) != instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareLess32: // A2
                      conditions_check |=
                        (DoMemoryRead<u32>(instructions[index].address) < instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareGreater32: // A3
                      conditions_check |=
                        (DoMemoryRead<u32>(instructions[index].address) > instructions[index].value32);
                      break;
                    case InstructionCode::ExtCompareBitsSet8: // E4 Internal to F6
                      conditions_check |=
                        (instructions[index].value8 ==
                         (DoMemoryRead<u8>(instructions[index].address) & instructions[index].value8));
                      break;
                    case InstructionCode::ExtCompareBitsClear8: // E5 Internal to F6
                      conditions_check |=
                        ((DoMemoryRead<u8>(instructions[index].address) & instructions[index].value8) == 0);
                      break;
                    case InstructionCode::ExtBitCompareButtons: // D7
                    {
                      const u32 frame_compare_value = instructions[index].address & 0xFFFFu;
                      const u8 cht_reg_no = Truncate8((instructions[index].value32 & 0xFF000000u) >> 24);
                      const bool bit_comparison_type = ((instructions[index].address & 0x100000u) >> 20);
                      const u8 frame_comparison = Truncate8((instructions[index].address & 0xF0000u) >> 16);
                      const u32 check_value = (instructions[index].value32 & 0xFFFFFFu);
                      const u32 value1 = GetControllerButtonBits();
                      const u32 value2 = GetControllerAnalogBits();
                      u32 value = value1 | value2;

                      if ((bit_comparison_type == false && check_value == (value & check_value)) // Check Bits are set
                          ||
                          (bit_comparison_type == true && check_value != (value & check_value))) // Check Bits are clear
                      {
                        cht_register[cht_reg_no] += 1;
                        switch (frame_comparison)
                        {
                          case 0x0: // No comparison on frame count, just do it
                            conditions_check |= true;
                            break;
                          case 0x1: // Check if frame_compare_value == current count
                            conditions_check |= (cht_register[cht_reg_no] == frame_compare_value);
                            break;
                          case 0x2: // Check if frame_compare_value < current count
                            conditions_check |= (cht_register[cht_reg_no] < frame_compare_value);
                            break;
                          case 0x3: // Check if frame_compare_value > current count
                            conditions_check |= (cht_register[cht_reg_no] > frame_compare_value);
                            break;
                          case 0x4: // Check if frame_compare_value != current count
                            conditions_check |= (cht_register[cht_reg_no] != frame_compare_value);
                            break;
                          default:
                            conditions_check |= false;
                            break;
                        }
                      }
                      else
                      {
                        cht_register[cht_reg_no] = 0;
                        conditions_check |= false;
                      }
                      break;
                    }
                    default:
                      Log_ErrorPrintf("Incorrect conditional instruction (see chtdb.txt for supported instructions)");
                      return;
                  }
                }
              }
              else
              {
                Log_ErrorPrintf("Incomplete multi conditional instruction");
                return;
              }
              if (conditions_check == true)
              {
                activate_codes = true;
                break;
              }
              else
              { // parse through to 00000000 FFFF and peek if next line is a F6 type associated with a ELSE
                activate_codes = false;
                // skip to the next separator (00000000 FFFF), or end
                constexpr u64 separator_value = UINT64_C(0x000000000000FFFF);
                constexpr u64 else_value = UINT64_C(0x00000000E15E0000);
                constexpr u64 elseif_value = UINT64_C(0x00000000E15E1F00);
                while (index < count)
                {
                  const u64 bits = instructions[index++].bits;
                  if (bits == separator_value)
                  {
                    const u64 bits_ahead = instructions[index].bits;
                    if ((bits_ahead & 0xFFFFFF00u) == elseif_value)
                    {
                      break;
                    }
                    if ((bits_ahead & 0xFFFF0000u) == else_value)
                    {
                      // index++;
                      activate_codes = true;
                      break;
                    }
                    index--;
                    break;
                  }
                  if ((bits & 0xFFFFFF00u) == elseif_value)
                  {
                    // index--;
                    break;
                  }
                  if ((bits & 0xFFFFFFFFu) == else_value)
                  {
                    // index++;
                    activate_codes = true;
                    break;
                  }
                }
                if (activate_codes == true)
                  break;
              }
            }
            break;
          }
          default:
            activate_codes = false;
            break;
        }

        if (activate_codes)
        {
          // execute following instructions
          continue;
        }

        // skip to the next separator (00000000 FFFF), or end
        constexpr u64 separator_value = UINT64_C(0x000000000000FFFF);
        while (index < count)
        {
          // we don't want to execute the separator instruction
          const u64 bits = instructions[index++].bits;
          if (bits == separator_value)
            break;
        }
      }
      break;

      case InstructionCode::ExtBitCompareButtons: // D7
      {
        index++;
        bool activate_codes;
        const u32 frame_compare_value = inst.address & 0xFFFFu;
        const u8 cht_reg_no = Truncate8((inst.value32 & 0xFF000000u) >> 24);
        const bool bit_comparison_type = ((inst.address & 0x100000u) >> 20);
        const u8 frame_comparison = Truncate8((inst.address & 0xF0000u) >> 16);
        const u32 check_value = (inst.value32 & 0xFFFFFFu);
        const u32 value1 = GetControllerButtonBits();
        const u32 value2 = GetControllerAnalogBits();
        u32 value = value1 | value2;

        if ((bit_comparison_type == false && check_value == (value & check_value))    // Check Bits are set
            || (bit_comparison_type == true && check_value != (value & check_value))) // Check Bits are clear
        {
          cht_register[cht_reg_no] += 1;
          switch (frame_comparison)
          {
            case 0x0: // No comparison on frame count, just do it
              activate_codes = true;
              break;
            case 0x1: // Check if frame_compare_value == current count
              activate_codes = (cht_register[cht_reg_no] == frame_compare_value);
              break;
            case 0x2: // Check if frame_compare_value < current count
              activate_codes = (cht_register[cht_reg_no] < frame_compare_value);
              break;
            case 0x3: // Check if frame_compare_value > current count
              activate_codes = (cht_register[cht_reg_no] > frame_compare_value);
              break;
            case 0x4: // Check if frame_compare_value != current count
              activate_codes = (cht_register[cht_reg_no] != frame_compare_value);
              break;
            default:
              activate_codes = false;
              break;
          }
        }
        else
        {
          cht_register[cht_reg_no] = 0;
          activate_codes = false;
        }

        if (activate_codes)
        {
          // execute following instructions
          continue;
        }

        // skip to the next separator (00000000 FFFF), or end
        constexpr u64 separator_value = UINT64_C(0x000000000000FFFF);
        while (index < count)
        {
          // we don't want to execute the separator instruction
          const u64 bits = instructions[index++].bits;
          if (bits == separator_value)
            break;
        }
      }
      break;

      case InstructionCode::ExtCheatRegistersCompare: // 52
      {
        index++;
        bool activate_codes = false;
        const u8 cht_reg_no1 = Truncate8(inst.address & 0xFFu);
        const u8 cht_reg_no2 = Truncate8((inst.address & 0xFF00u) >> 8);
        const u8 sub_type = Truncate8((inst.first & 0xFF0000u) >> 16);

        switch (sub_type)
        {
          case 0x00:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) == Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x01:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) != Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x02:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) > Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x03:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) >= Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x04:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) < Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x05:
            activate_codes =
              (Truncate8(cht_register[cht_reg_no2] & 0xFFu) <= Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x06:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no2] & 0xFFu) & Truncate8(cht_register[cht_reg_no1] & 0xFFu)) ==
               (Truncate8(cht_register[cht_reg_no1] & 0xFFu)));
            break;
          case 0x07:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no2] & 0xFFu) & Truncate8(cht_register[cht_reg_no1] & 0xFFu)) !=
               (Truncate8(cht_register[cht_reg_no1] & 0xFFu)));
            break;
          case 0x0A:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no2] & 0xFFu) & Truncate8(cht_register[cht_reg_no1] & 0xFFu)) ==
               (Truncate8(cht_register[cht_reg_no2] & 0xFFu)));
            break;
          case 0x0B:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no2] & 0xFFu) & Truncate8(cht_register[cht_reg_no1] & 0xFFu)) !=
               (Truncate8(cht_register[cht_reg_no2] & 0xFFu)));
            break;
          case 0x10:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) == inst.value8);
            break;
          case 0x11:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) != inst.value8);
            break;
          case 0x12:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) > inst.value8);
            break;
          case 0x13:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) >= inst.value8);
            break;
          case 0x14:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) < inst.value8);
            break;
          case 0x15:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) <= inst.value8);
            break;
          case 0x16:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & inst.value8) == inst.value8);
            break;
          case 0x17:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & inst.value8) != inst.value8);
            break;
          case 0x18:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) > inst.value8) &&
               (Truncate8(cht_register[cht_reg_no1] & 0xFFu) < Truncate8((inst.value32 & 0xFF0000u) >> 16)));
            break;
          case 0x19:
            activate_codes =
              ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) >= inst.value8) &&
               (Truncate8(cht_register[cht_reg_no1] & 0xFFu) <= Truncate8((inst.value32 & 0xFF0000u) >> 16)));
            break;
          case 0x1A:
            activate_codes = ((Truncate8(cht_register[cht_reg_no2] & 0xFFu) & inst.value8) ==
                              Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x1B:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & inst.value8) !=
                              Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x20:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) == DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x21:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) != DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x22:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) > DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x23:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) >= DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x24:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) < DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x25:
            activate_codes =
              (DoMemoryRead<u8>(cht_register[cht_reg_no2]) <= DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x26:
            activate_codes = ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) & inst.value8) == inst.value8);
            break;
          case 0x27:
            activate_codes = ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) & inst.value8) != inst.value8);
            break;
          case 0x28:
            activate_codes =
              ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) > inst.value8) &&
               (DoMemoryRead<u8>(cht_register[cht_reg_no1]) < Truncate8((inst.value32 & 0xFF0000u) >> 16)));
            break;
          case 0x29:
            activate_codes =
              ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) >= inst.value8) &&
               (DoMemoryRead<u8>(cht_register[cht_reg_no1]) <= Truncate8((inst.value32 & 0xFF0000u) >> 16)));
            break;
          case 0x2A:
            activate_codes = ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) & inst.value8) ==
                              DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x2B:
            activate_codes = ((DoMemoryRead<u8>(cht_register[cht_reg_no1]) & inst.value8) !=
                              DoMemoryRead<u8>(cht_register[cht_reg_no1]));
            break;
          case 0x30:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) == DoMemoryRead<u8>(inst.value32));
            break;
          case 0x31:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) != DoMemoryRead<u8>(inst.value32));
            break;
          case 0x32:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) > DoMemoryRead<u8>(inst.value32));
            break;
          case 0x33:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) >= DoMemoryRead<u8>(inst.value32));
            break;
          case 0x34:
            activate_codes = (Truncate8(cht_register[cht_reg_no1] & 0xFFu) < DoMemoryRead<u8>(inst.value32));
            break;
          case 0x36:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & DoMemoryRead<u8>(inst.value32)) ==
                              DoMemoryRead<u8>(inst.value32));
            break;
          case 0x37:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & DoMemoryRead<u8>(inst.value32)) !=
                              DoMemoryRead<u8>(inst.value32));
            break;
          case 0x3A:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & DoMemoryRead<u8>(inst.value32)) ==
                              Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x3B:
            activate_codes = ((Truncate8(cht_register[cht_reg_no1] & 0xFFu) & DoMemoryRead<u8>(inst.value32)) !=
                              Truncate8(cht_register[cht_reg_no1] & 0xFFu));
            break;
          case 0x40:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) == Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x41:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) != Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x42:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) > Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x43:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) >= Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x44:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) < Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x45:
            activate_codes =
              (Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) <= Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x46:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) & Truncate16(cht_register[cht_reg_no1] & 0xFFFFu)) ==
               Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x47:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) & Truncate16(cht_register[cht_reg_no1] & 0xFFFFu)) !=
               Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x4A:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) & Truncate16(cht_register[cht_reg_no1] & 0xFFFFu)) ==
               Truncate16(cht_register[cht_reg_no2] & 0xFFFFu));
            break;
          case 0x4B:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) & Truncate16(cht_register[cht_reg_no1] & 0xFFFFu)) !=
               Truncate16(cht_register[cht_reg_no2] & 0xFFFFu));
            break;
          case 0x50:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) == inst.value16);
            break;
          case 0x51:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) != inst.value16);
            break;
          case 0x52:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) > inst.value16);
            break;
          case 0x53:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) >= inst.value16);
            break;
          case 0x54:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) < inst.value16);
            break;
          case 0x55:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) <= inst.value16);
            break;
          case 0x56:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & inst.value16) == inst.value16);
            break;
          case 0x57:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & inst.value16) != inst.value16);
            break;
          case 0x58:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) > inst.value16) &&
               (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) < Truncate16((inst.value32 & 0xFFFF0000u) >> 16)));
            break;
          case 0x59:
            activate_codes =
              ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) >= inst.value16) &&
               (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) <= Truncate16((inst.value32 & 0xFFFF0000u) >> 16)));
            break;
          case 0x5A:
            activate_codes = ((Truncate16(cht_register[cht_reg_no2] & 0xFFFFu) & inst.value16) ==
                              Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x5B:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & inst.value16) !=
                              Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x60:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) == DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x61:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) != DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x62:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) > DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x63:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) >= DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x64:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) < DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x65:
            activate_codes =
              (DoMemoryRead<u16>(cht_register[cht_reg_no2]) <= DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x66:
            activate_codes = ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) & inst.value16) == inst.value16);
            break;
          case 0x67:
            activate_codes = ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) & inst.value16) != inst.value16);
            break;
          case 0x68:
            activate_codes =
              ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) > inst.value16) &&
               (DoMemoryRead<u16>(cht_register[cht_reg_no1]) < Truncate16((inst.value32 & 0xFFFF0000u) >> 16)));
            break;
          case 0x69:
            activate_codes =
              ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) >= inst.value16) &&
               (DoMemoryRead<u16>(cht_register[cht_reg_no1]) <= Truncate16((inst.value32 & 0xFFFF0000u) >> 16)));
            break;
          case 0x6A:
            activate_codes = ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) & inst.value16) ==
                              DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x6B:
            activate_codes = ((DoMemoryRead<u16>(cht_register[cht_reg_no1]) & inst.value16) !=
                              DoMemoryRead<u16>(cht_register[cht_reg_no1]));
            break;
          case 0x70:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) == DoMemoryRead<u16>(inst.value32));
            break;
          case 0x71:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) != DoMemoryRead<u16>(inst.value32));
            break;
          case 0x72:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) > DoMemoryRead<u16>(inst.value32));
            break;
          case 0x73:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) >= DoMemoryRead<u16>(inst.value32));
            break;
          case 0x74:
            activate_codes = (Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) < DoMemoryRead<u16>(inst.value32));
            break;
          case 0x76:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & DoMemoryRead<u16>(inst.value32)) ==
                              DoMemoryRead<u16>(inst.value32));
            break;
          case 0x77:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & DoMemoryRead<u16>(inst.value32)) !=
                              DoMemoryRead<u16>(inst.value32));
            break;
          case 0x7A:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & DoMemoryRead<u16>(inst.value32)) ==
                              Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x7B:
            activate_codes = ((Truncate16(cht_register[cht_reg_no1] & 0xFFFFu) & DoMemoryRead<u16>(inst.value32)) !=
                              Truncate16(cht_register[cht_reg_no1] & 0xFFFFu));
            break;
          case 0x80:
            activate_codes = (cht_register[cht_reg_no2] == cht_register[cht_reg_no1]);
            break;
          case 0x81:
            activate_codes = (cht_register[cht_reg_no2] != cht_register[cht_reg_no1]);
            break;
          case 0x82:
            activate_codes = (cht_register[cht_reg_no2] > cht_register[cht_reg_no1]);
            break;
          case 0x83:
            activate_codes = (cht_register[cht_reg_no2] >= cht_register[cht_reg_no1]);
            break;
          case 0x84:
            activate_codes = (cht_register[cht_reg_no2] < cht_register[cht_reg_no1]);
            break;
          case 0x85:
            activate_codes = (cht_register[cht_reg_no2] <= cht_register[cht_reg_no1]);
            break;
          case 0x86:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) == cht_register[cht_reg_no1]);
            break;
          case 0x87:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) != cht_register[cht_reg_no1]);
            break;
          case 0x8A:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) == cht_register[cht_reg_no2]);
            break;
          case 0x8B:
            activate_codes = ((cht_register[cht_reg_no2] & cht_register[cht_reg_no1]) != cht_register[cht_reg_no2]);
            break;
          case 0x90:
            activate_codes = (cht_register[cht_reg_no1] == inst.value32);
            break;
          case 0x91:
            activate_codes = (cht_register[cht_reg_no1] != inst.value32);
            break;
          case 0x92:
            activate_codes = (cht_register[cht_reg_no1] > inst.value32);
            break;
          case 0x93:
            activate_codes = (cht_register[cht_reg_no1] >= inst.value32);
            break;
          case 0x94:
            activate_codes = (cht_register[cht_reg_no1] < inst.value32);
            break;
          case 0x95:
            activate_codes = (cht_register[cht_reg_no1] <= inst.value32);
            break;
          case 0x96:
            activate_codes = ((cht_register[cht_reg_no1] & inst.value32) == inst.value32);
            break;
          case 0x97:
            activate_codes = ((cht_register[cht_reg_no1] & inst.value32) != inst.value32);
            break;
          case 0x9A:
            activate_codes = ((cht_register[cht_reg_no2] & inst.value32) == cht_register[cht_reg_no1]);
            break;
          case 0x9B:
            activate_codes = ((cht_register[cht_reg_no1] & inst.value32) != cht_register[cht_reg_no1]);
            break;
          case 0xA0:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) == DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA1:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) != DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA2:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) > DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA3:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) >= DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA4:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) < DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA5:
            activate_codes =
              (DoMemoryRead<u32>(cht_register[cht_reg_no2]) <= DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xA6:
            activate_codes = ((DoMemoryRead<u32>(cht_register[cht_reg_no1]) & inst.value32) == inst.value32);
            break;
          case 0xA7:
            activate_codes = ((DoMemoryRead<u32>(cht_register[cht_reg_no1]) & inst.value32) != inst.value32);
            break;
          case 0xAA:
            activate_codes = ((DoMemoryRead<u32>(cht_register[cht_reg_no1]) & inst.value32) ==
                              DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xAB:
            activate_codes = ((DoMemoryRead<u32>(cht_register[cht_reg_no1]) & inst.value32) !=
                              DoMemoryRead<u32>(cht_register[cht_reg_no1]));
            break;
          case 0xB0:
            activate_codes = (cht_register[cht_reg_no1] == DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB1:
            activate_codes = (cht_register[cht_reg_no1] != DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB2:
            activate_codes = (cht_register[cht_reg_no1] > DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB3:
            activate_codes = (cht_register[cht_reg_no1] >= DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB4:
            activate_codes = (cht_register[cht_reg_no1] < DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB6:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<u32>(inst.value32)) == DoMemoryRead<u32>(inst.value32));
            break;
          case 0xB7:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<u32>(inst.value32)) != DoMemoryRead<u32>(inst.value32));
            break;
          case 0xBA:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<u32>(inst.value32)) == cht_register[cht_reg_no1]);
            break;
          case 0xBB:
            activate_codes =
              ((cht_register[cht_reg_no1] & DoMemoryRead<u32>(inst.value32)) != cht_register[cht_reg_no1]);
            break;
          default:
            activate_codes = false;
            break;
        }
        if (activate_codes)
        {
          // execute following instructions
          continue;
        }

        // skip to the next separator (00000000 FFFF), or end
        constexpr u64 separator_value = UINT64_C(0x000000000000FFFF);
        while (index < count)
        {
          // we don't want to execute the separator instruction
          const u64 bits = instructions[index++].bits;
          if (bits == separator_value)
            break;
        }
      }
      break;

      case InstructionCode::DelayActivation: // C1
      {
        // A value of around 4000 or 5000 will usually give you a good 20-30 second delay before codes are activated.
        // Frame number * 0.3 -> (20 * 60) * 10 / 3 => 4000
        const u32 comp_value = (System::GetFrameNumber() * 10) / 3;
        if (comp_value < inst.value16)
          index = count;
        else
          index++;
      }
      break;

      case InstructionCode::Slide:
      {
        if ((index + 1) >= instructions.size())
        {
          Log_ErrorPrintf("Incomplete slide instruction");
          return;
        }

        const u32 slide_count = (inst.first >> 8) & 0xFFu;
        const u32 address_increment = inst.first & 0xFFu;
        const u16 value_increment = Truncate16(inst.second);
        const Instruction& inst2 = instructions[index + 1];
        const InstructionCode write_type = inst2.code;
        u32 address = inst2.address;
        u16 value = inst2.value16;

        if (write_type == InstructionCode::ConstantWrite8)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u8>(address, Truncate8(value));
            address += address_increment;
            value += value_increment;
          }
        }
        else if (write_type == InstructionCode::ConstantWrite16)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u16>(address, value);
            address += address_increment;
            value += value_increment;
          }
        }
        else
        {
          Log_ErrorPrintf("Invalid command in second slide parameter 0x%02X", static_cast<unsigned>(write_type));
        }

        index += 2;
      }
      break;

      case InstructionCode::ExtImprovedSlide:
      {
        if ((index + 1) >= instructions.size())
        {
          Log_ErrorPrintf("Incomplete slide instruction");
          return;
        }

        const u32 slide_count = inst.first & 0xFFFFu;
        const u32 address_change = (inst.second >> 16) & 0xFFFFu;
        const u16 value_change = Truncate16(inst.second);
        const Instruction& inst2 = instructions[index + 1];
        const InstructionCode write_type = inst2.code;
        const bool address_change_negative = (inst.first >> 20) & 0x1u;
        const bool value_change_negative = (inst.first >> 16) & 0x1u;
        u32 address = inst2.address;
        u32 value = inst2.value32;

        if (write_type == InstructionCode::ConstantWrite8)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u8>(address, Truncate8(value));
            if (address_change_negative)
              address -= address_change;
            else
              address += address_change;
            if (value_change_negative)
              value -= value_change;
            else
              value += value_change;
          }
        }
        else if (write_type == InstructionCode::ConstantWrite16)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u16>(address, Truncate16(value));
            if (address_change_negative)
              address -= address_change;
            else
              address += address_change;
            if (value_change_negative)
              value -= value_change;
            else
              value += value_change;
          }
        }
        else if (write_type == InstructionCode::ExtConstantWrite32)
        {
          for (u32 i = 0; i < slide_count; i++)
          {
            DoMemoryWrite<u32>(address, value);
            if (address_change_negative)
              address -= address_change;
            else
              address += address_change;
            if (value_change_negative)
              value -= value_change;
            else
              value += value_change;
          }
        }
        else
        {
          Log_ErrorPrintf("Invalid command in second slide parameter 0x%02X", static_cast<unsigned>(write_type));
        }

        index += 2;
      }
      break;

      case InstructionCode::MemoryCopy:
      {
        if ((index + 1) >= instructions.size())
        {
          Log_ErrorPrintf("Incomplete memory copy instruction");
          return;
        }

        const Instruction& inst2 = instructions[index + 1];
        const u32 byte_count = inst.value16;
        u32 src_address = inst.address;
        u32 dst_address = inst2.address;

        for (u32 i = 0; i < byte_count; i++)
        {
          u8 value = DoMemoryRead<u8>(src_address);
          DoMemoryWrite<u8>(dst_address, value);
          src_address++;
          dst_address++;
        }

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

void CheatCode::ApplyOnDisable() const
{
  const u32 count = static_cast<u32>(instructions.size());
  u32 index = 0;
  for (; index < count;)
  {
    const Instruction& inst = instructions[index];
    switch (inst.code)
    {
      case InstructionCode::Nop:
      case InstructionCode::ConstantWrite8:
      case InstructionCode::ConstantWrite16:
      case InstructionCode::ExtConstantWrite32:
      case InstructionCode::ExtConstantBitSet8:
      case InstructionCode::ExtConstantBitSet16:
      case InstructionCode::ExtConstantBitSet32:
      case InstructionCode::ExtConstantBitClear8:
      case InstructionCode::ExtConstantBitClear16:
      case InstructionCode::ExtConstantBitClear32:
      case InstructionCode::ScratchpadWrite16:
      case InstructionCode::ExtScratchpadWrite32:
      case InstructionCode::ExtIncrement32:
      case InstructionCode::ExtDecrement32:
      case InstructionCode::Increment16:
      case InstructionCode::Decrement16:
      case InstructionCode::Increment8:
      case InstructionCode::Decrement8:
      case InstructionCode::ExtConstantForceRange8:
      case InstructionCode::ExtConstantForceRangeLimits16:
      case InstructionCode::ExtConstantForceRangeRollRound16:
      case InstructionCode::ExtConstantSwap16:
      case InstructionCode::DelayActivation: // C1
      case InstructionCode::ExtConstantWriteIfMatch16:
      case InstructionCode::ExtCheatRegisters:
        index++;
        break;

      case InstructionCode::ExtConstantForceRange16:
      case InstructionCode::Slide:
      case InstructionCode::ExtImprovedSlide:
      case InstructionCode::MemoryCopy:
        index += 2;
        break;
      case InstructionCode::ExtFindAndReplace:
        index += 5;
        break;
      // for conditionals, we don't want to skip over in case it changed at some point
      case InstructionCode::ExtCompareEqual32:
      case InstructionCode::ExtCompareNotEqual32:
      case InstructionCode::ExtCompareLess32:
      case InstructionCode::ExtCompareGreater32:
      case InstructionCode::CompareEqual16:
      case InstructionCode::CompareNotEqual16:
      case InstructionCode::CompareLess16:
      case InstructionCode::CompareGreater16:
      case InstructionCode::CompareEqual8:
      case InstructionCode::CompareNotEqual8:
      case InstructionCode::CompareLess8:
      case InstructionCode::CompareGreater8:
      case InstructionCode::CompareButtons: // D4
        index++;
        break;

      // same deal for block conditionals
      case InstructionCode::SkipIfNotEqual16:         // C0
      case InstructionCode::ExtSkipIfNotEqual32:      // A4
      case InstructionCode::SkipIfButtonsNotEqual:    // D5
      case InstructionCode::SkipIfButtonsEqual:       // D6
      case InstructionCode::ExtBitCompareButtons:     // D7
      case InstructionCode::ExtSkipIfNotLess8:        // C3
      case InstructionCode::ExtSkipIfNotGreater8:     // C4
      case InstructionCode::ExtSkipIfNotLess16:       // C5
      case InstructionCode::ExtSkipIfNotGreater16:    // C6
      case InstructionCode::ExtMultiConditionals:     // F6
      case InstructionCode::ExtCheatRegistersCompare: // 52
        index++;
        break;

      case InstructionCode::ExtConstantWriteIfMatchWithRestore16:
      {
        const u16 value = DoMemoryRead<u16>(inst.address);
        const u16 comparevalue = Truncate16(inst.value32 >> 16);
        const u16 newvalue = Truncate16(inst.value32 & 0xFFFFu);
        if (value == newvalue)
          DoMemoryWrite<u16>(inst.address, comparevalue);

        index++;
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

static std::array<const char*, 1> s_cheat_code_type_names = {{"Gameshark"}};
static std::array<const char*, 1> s_cheat_code_type_display_names{{TRANSLATABLE("Cheats", "Gameshark")}};

const char* CheatCode::GetTypeName(Type type)
{
  return s_cheat_code_type_names[static_cast<u32>(type)];
}

const char* CheatCode::GetTypeDisplayName(Type type)
{
  return s_cheat_code_type_display_names[static_cast<u32>(type)];
}

std::optional<CheatCode::Type> CheatCode::ParseTypeName(const char* str)
{
  for (u32 i = 0; i < static_cast<u32>(s_cheat_code_type_names.size()); i++)
  {
    if (std::strcmp(s_cheat_code_type_names[i], str) == 0)
      return static_cast<Type>(i);
  }

  return std::nullopt;
}

static std::array<const char*, 2> s_cheat_code_activation_names = {{"Manual", "EndFrame"}};
static std::array<const char*, 2> s_cheat_code_activation_display_names{
  {TRANSLATABLE("Cheats", "Manual"), TRANSLATABLE("Cheats", "Automatic (Frame End)")}};

const char* CheatCode::GetActivationName(Activation activation)
{
  return s_cheat_code_activation_names[static_cast<u32>(activation)];
}

const char* CheatCode::GetActivationDisplayName(Activation activation)
{
  return s_cheat_code_activation_display_names[static_cast<u32>(activation)];
}

std::optional<CheatCode::Activation> CheatCode::ParseActivationName(const char* str)
{
  for (u32 i = 0; i < static_cast<u32>(s_cheat_code_activation_names.size()); i++)
  {
    if (std::strcmp(s_cheat_code_activation_names[i], str) == 0)
      return static_cast<Activation>(i);
  }

  return std::nullopt;
}

MemoryScan::MemoryScan() = default;

MemoryScan::~MemoryScan() = default;

void MemoryScan::ResetSearch()
{
  m_results.clear();
}

void MemoryScan::Search()
{
  m_results.clear();

  switch (m_size)
  {
    case MemoryAccessSize::Byte:
      SearchBytes();
      break;

    case MemoryAccessSize::HalfWord:
      SearchHalfwords();
      break;

    case MemoryAccessSize::Word:
      SearchWords();
      break;

    default:
      break;
  }
}

void MemoryScan::SearchBytes()
{
  for (PhysicalMemoryAddress address = m_start_address; address < m_end_address; address++)
  {
    if (!IsValidScanAddress(address))
      continue;

    const u8 bvalue = DoMemoryRead<u8>(address);

    Result res;
    res.address = address;
    res.value = m_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    res.last_value = res.value;
    res.value_changed = false;

    if (res.Filter(m_operator, m_value, m_signed))
      m_results.push_back(res);
  }
}

void MemoryScan::SearchHalfwords()
{
  for (PhysicalMemoryAddress address = m_start_address; address < m_end_address; address += 2)
  {
    if (!IsValidScanAddress(address))
      continue;

    const u16 bvalue = DoMemoryRead<u16>(address);

    Result res;
    res.address = address;
    res.value = m_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    res.last_value = res.value;
    res.value_changed = false;

    if (res.Filter(m_operator, m_value, m_signed))
      m_results.push_back(res);
  }
}

void MemoryScan::SearchWords()
{
  for (PhysicalMemoryAddress address = m_start_address; address < m_end_address; address += 4)
  {
    if (!IsValidScanAddress(address))
      continue;

    Result res;
    res.address = address;
    res.value = DoMemoryRead<u32>(address);
    res.last_value = res.value;
    res.value_changed = false;

    if (res.Filter(m_operator, m_value, m_signed))
      m_results.push_back(res);
  }
}

void MemoryScan::SearchAgain()
{
  ResultVector new_results;
  new_results.reserve(m_results.size());
  for (Result& res : m_results)
  {
    res.UpdateValue(m_size, m_signed);

    if (res.Filter(m_operator, m_value, m_signed))
    {
      res.last_value = res.value;
      new_results.push_back(res);
    }
  }

  m_results.swap(new_results);
}

void MemoryScan::UpdateResultsValues()
{
  for (Result& res : m_results)
    res.UpdateValue(m_size, m_signed);
}

void MemoryScan::SetResultValue(u32 index, u32 value)
{
  if (index >= m_results.size())
    return;

  Result& res = m_results[index];
  if (res.value == value)
    return;

  switch (m_size)
  {
    case MemoryAccessSize::Byte:
      DoMemoryWrite<u8>(res.address, Truncate8(value));
      break;

    case MemoryAccessSize::HalfWord:
      DoMemoryWrite<u16>(res.address, Truncate16(value));
      break;

    case MemoryAccessSize::Word:
      CPU::SafeWriteMemoryWord(res.address, value);
      break;
  }

  res.value = value;
  res.value_changed = true;
}

bool MemoryScan::Result::Filter(Operator op, u32 comp_value, bool is_signed) const
{
  switch (op)
  {
    case Operator::Equal:
    {
      return (value == comp_value);
    }

    case Operator::NotEqual:
    {
      return (value != comp_value);
    }

    case Operator::GreaterThan:
    {
      return is_signed ? (static_cast<s32>(value) > static_cast<s32>(comp_value)) : (value > comp_value);
    }

    case Operator::GreaterEqual:
    {
      return is_signed ? (static_cast<s32>(value) >= static_cast<s32>(comp_value)) : (value >= comp_value);
    }

    case Operator::LessThan:
    {
      return is_signed ? (static_cast<s32>(value) < static_cast<s32>(comp_value)) : (value < comp_value);
    }

    case Operator::LessEqual:
    {
      return is_signed ? (static_cast<s32>(value) <= static_cast<s32>(comp_value)) : (value <= comp_value);
    }

    case Operator::IncreasedBy:
    {
      return is_signed ? ((static_cast<s32>(value) - static_cast<s32>(last_value)) == static_cast<s32>(comp_value)) :
                         ((value - last_value) == comp_value);
    }

    case Operator::DecreasedBy:
    {
      return is_signed ? ((static_cast<s32>(last_value) - static_cast<s32>(value)) == static_cast<s32>(comp_value)) :
                         ((last_value - value) == comp_value);
    }

    case Operator::ChangedBy:
    {
      if (is_signed)
        return (std::abs(static_cast<s32>(last_value) - static_cast<s32>(value)) == static_cast<s32>(comp_value));
      else
        return ((last_value > value) ? (last_value - value) : (value - last_value)) == comp_value;
    }

    case Operator::EqualLast:
    {
      return (value == last_value);
    }

    case Operator::NotEqualLast:
    {
      return (value != last_value);
    }

    case Operator::GreaterThanLast:
    {
      return is_signed ? (static_cast<s32>(value) > static_cast<s32>(last_value)) : (value > last_value);
    }

    case Operator::GreaterEqualLast:
    {
      return is_signed ? (static_cast<s32>(value) >= static_cast<s32>(last_value)) : (value >= last_value);
    }

    case Operator::LessThanLast:
    {
      return is_signed ? (static_cast<s32>(value) < static_cast<s32>(last_value)) : (value < last_value);
    }

    case Operator::LessEqualLast:
    {
      return is_signed ? (static_cast<s32>(value) <= static_cast<s32>(last_value)) : (value <= last_value);
    }

    case Operator::Any:
      return true;

    default:
      return false;
  }
}

void MemoryScan::Result::UpdateValue(MemoryAccessSize size, bool is_signed)
{
  const u32 old_value = value;

  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      u8 bvalue = DoMemoryRead<u8>(address);
      value = is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::HalfWord:
    {
      u16 bvalue = DoMemoryRead<u16>(address);
      value = is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::Word:
    {
      CPU::SafeReadMemoryWord(address, &value);
    }
    break;
  }

  value_changed = (value != old_value);
}

MemoryWatchList::MemoryWatchList() = default;

MemoryWatchList::~MemoryWatchList() = default;

const MemoryWatchList::Entry* MemoryWatchList::GetEntryByAddress(u32 address) const
{
  for (const Entry& entry : m_entries)
  {
    if (entry.address == address)
      return &entry;
  }

  return nullptr;
}

bool MemoryWatchList::AddEntry(std::string description, u32 address, MemoryAccessSize size, bool is_signed, bool freeze)
{
  if (GetEntryByAddress(address))
    return false;

  Entry entry;
  entry.description = std::move(description);
  entry.address = address;
  entry.size = size;
  entry.is_signed = is_signed;
  entry.freeze = false;

  UpdateEntryValue(&entry);

  entry.changed = false;
  entry.freeze = freeze;

  m_entries.push_back(std::move(entry));
  return true;
}

void MemoryWatchList::RemoveEntry(u32 index)
{
  if (index >= m_entries.size())
    return;

  m_entries.erase(m_entries.begin() + index);
}

bool MemoryWatchList::RemoveEntryByAddress(u32 address)
{
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
  {
    if (it->address == address)
    {
      m_entries.erase(it);
      return true;
    }
  }

  return false;
}

void MemoryWatchList::SetEntryDescription(u32 index, std::string description)
{
  if (index >= m_entries.size())
    return;

  Entry& entry = m_entries[index];
  entry.description = std::move(description);
}

void MemoryWatchList::SetEntryFreeze(u32 index, bool freeze)
{
  if (index >= m_entries.size())
    return;

  Entry& entry = m_entries[index];
  entry.freeze = freeze;
}

void MemoryWatchList::SetEntryValue(u32 index, u32 value)
{
  if (index >= m_entries.size())
    return;

  Entry& entry = m_entries[index];
  if (entry.value == value)
    return;

  SetEntryValue(&entry, value);
}

bool MemoryWatchList::RemoveEntryByDescription(const char* description)
{
  bool result = false;
  for (auto it = m_entries.begin(); it != m_entries.end();)
  {
    if (it->description == description)
    {
      it = m_entries.erase(it);
      result = true;
      continue;
    }

    ++it;
  }

  return result;
}

void MemoryWatchList::UpdateValues()
{
  for (Entry& entry : m_entries)
    UpdateEntryValue(&entry);
}

void MemoryWatchList::SetEntryValue(Entry* entry, u32 value)
{
  switch (entry->size)
  {
    case MemoryAccessSize::Byte:
      DoMemoryWrite<u8>(entry->address, Truncate8(value));
      break;

    case MemoryAccessSize::HalfWord:
      DoMemoryWrite<u16>(entry->address, Truncate16(value));
      break;

    case MemoryAccessSize::Word:
      DoMemoryWrite<u32>(entry->address, value);
      break;
  }

  entry->changed = (entry->value != value);
  entry->value = value;
}

void MemoryWatchList::UpdateEntryValue(Entry* entry)
{
  const u32 old_value = entry->value;

  switch (entry->size)
  {
    case MemoryAccessSize::Byte:
    {
      u8 bvalue = DoMemoryRead<u8>(entry->address);
      entry->value = entry->is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::HalfWord:
    {
      u16 bvalue = DoMemoryRead<u16>(entry->address);
      entry->value = entry->is_signed ? SignExtend32(bvalue) : ZeroExtend32(bvalue);
    }
    break;

    case MemoryAccessSize::Word:
    {
      entry->value = DoMemoryRead<u32>(entry->address);
    }
    break;
  }

  entry->changed = (old_value != entry->value);

  if (entry->freeze && entry->changed)
    SetEntryValue(entry, old_value);
}
