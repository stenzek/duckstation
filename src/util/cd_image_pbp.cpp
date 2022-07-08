#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "pbp_types.h"
#include "string.h"
#include "zlib.h"
#include <array>
#include <cstdio>
#include <vector>
Log_SetChannel(CDImagePBP);

using namespace PBP;
using FileSystem::FSeek64;
using FileSystem::FTell64;

class CDImagePBP final : public CDImage
{
public:
  CDImagePBP() = default;
  ~CDImagePBP() override;

  bool Open(const char* filename, Common::Error* error);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

  bool HasSubImages() const override;
  u32 GetSubImageCount() const override;
  u32 GetCurrentSubImage() const override;
  bool SwitchSubImage(u32 index, Common::Error* error) override;
  std::string GetMetadata(const std::string_view& type) const override;
  std::string GetSubImageMetadata(u32 index, const std::string_view& type) const override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  struct BlockInfo
  {
    u32 offset; // Absolute offset from start of file
    u16 size;
  };

#if _DEBUG
  static void PrintPBPHeaderInfo(const PBPHeader& pbp_header);
  static void PrintSFOHeaderInfo(const SFOHeader& sfo_header);
  static void PrintSFOIndexTableEntry(const SFOIndexTableEntry& sfo_index_table_entry, size_t i);
  static void PrintSFOTable(const SFOTable& sfo_table);
#endif

  bool LoadPBPHeader();
  bool LoadSFOHeader();
  bool LoadSFOIndexTable();
  bool LoadSFOTable();

  bool IsValidEboot(Common::Error* error);

  bool InitDecompressionStream();
  bool DecompressBlock(const BlockInfo& block_info);

  bool OpenDisc(u32 index, Common::Error* error);

  static const std::string* LookupStringSFOTableEntry(const char* key, const SFOTable& table);

  FILE* m_file = nullptr;

  PBPHeader m_pbp_header;
  SFOHeader m_sfo_header;
  SFOTable m_sfo_table;
  SFOIndexTable m_sfo_index_table;

  // Absolute offsets to ISO headers, size is the number of discs in the file
  std::vector<u32> m_disc_offsets;
  u32 m_current_disc = 0;

  // Absolute offsets and sizes of blocks in m_file
  std::array<BlockInfo, BLOCK_TABLE_NUM_ENTRIES> m_blockinfo_table;

  std::array<TOCEntry, TOC_NUM_ENTRIES> m_toc;

  u32 m_current_block = static_cast<u32>(-1);
  std::array<u8, DECOMPRESSED_BLOCK_SIZE> m_decompressed_block;
  std::vector<u8> m_compressed_block;

  z_stream m_inflate_stream;

  CDSubChannelReplacement m_sbi;
};

namespace EndianHelper {
static constexpr bool HostIsLittleEndian()
{
  constexpr union
  {
    u8 a[4];
    u32 b;
  } test_val = {{1}};

  return test_val.a[0] == 1;
}

template<typename T>
static void SwapByteOrder(T& val)
{
  union
  {
    T t;
    std::array<u8, sizeof(T)> arr;
  } swap_val;

  swap_val.t = val;
  std::reverse(std::begin(swap_val.arr), std::end(swap_val.arr));
  val = swap_val.t;
}
} // namespace EndianHelper

CDImagePBP::~CDImagePBP()
{
  if (m_file)
    fclose(m_file);

  inflateEnd(&m_inflate_stream);
}

bool CDImagePBP::LoadPBPHeader()
{
  if (!m_file)
    return false;

  if (FSeek64(m_file, 0, SEEK_END) != 0)
    return false;

  if (FTell64(m_file) < 0)
    return false;

  if (FSeek64(m_file, 0, SEEK_SET) != 0)
    return false;

  if (fread(&m_pbp_header, sizeof(PBPHeader), 1, m_file) != 1)
  {
    Log_ErrorPrint("Unable to read PBP header");
    return false;
  }

  if (strncmp((char*)m_pbp_header.magic, "\0PBP", 4) != 0)
  {
    Log_ErrorPrint("PBP magic number mismatch");
    return false;
  }

#if _DEBUG
  PrintPBPHeaderInfo(m_pbp_header);
#endif

  return true;
}

bool CDImagePBP::LoadSFOHeader()
{
  if (FSeek64(m_file, m_pbp_header.param_sfo_offset, SEEK_SET) != 0)
    return false;

  if (fread(&m_sfo_header, sizeof(SFOHeader), 1, m_file) != 1)
    return false;

  if (strncmp((char*)m_sfo_header.magic, "\0PSF", 4) != 0)
  {
    Log_ErrorPrint("SFO magic number mismatch");
    return false;
  }

#if _DEBUG
  PrintSFOHeaderInfo(m_sfo_header);
#endif

  return true;
}

bool CDImagePBP::LoadSFOIndexTable()
{
  m_sfo_index_table.clear();
  m_sfo_index_table.resize(m_sfo_header.num_table_entries);

  if (FSeek64(m_file, m_pbp_header.param_sfo_offset + sizeof(m_sfo_header), SEEK_SET) != 0)
    return false;

  if (fread(m_sfo_index_table.data(), sizeof(SFOIndexTableEntry), m_sfo_header.num_table_entries, m_file) !=
      m_sfo_header.num_table_entries)
    return false;

#if _DEBUG
  for (size_t i = 0; i < static_cast<size_t>(m_sfo_header.num_table_entries); ++i)
    PrintSFOIndexTableEntry(m_sfo_index_table[i], i);
#endif

  return true;
}

bool CDImagePBP::LoadSFOTable()
{
  m_sfo_table.clear();

  for (size_t i = 0; i < static_cast<size_t>(m_sfo_header.num_table_entries); ++i)
  {
    u32 abs_key_offset =
      m_pbp_header.param_sfo_offset + m_sfo_header.key_table_offset + m_sfo_index_table[i].key_offset;
    u32 abs_data_offset =
      m_pbp_header.param_sfo_offset + m_sfo_header.data_table_offset + m_sfo_index_table[i].data_offset;

    if (FSeek64(m_file, abs_key_offset, SEEK_SET) != 0)
    {
      Log_ErrorPrintf("Failed seek to key for SFO table entry %zu", i);
      return false;
    }

    // Longest known key string is 20 characters total, including the null character
    char key_cstr[20] = {};
    if (fgets(key_cstr, sizeof(key_cstr), m_file) == nullptr)
    {
      Log_ErrorPrintf("Failed to read key string for SFO table entry %zu", i);
      return false;
    }

    if (FSeek64(m_file, abs_data_offset, SEEK_SET) != 0)
    {
      Log_ErrorPrintf("Failed seek to data for SFO table entry %zu", i);
      return false;
    }

    if (m_sfo_index_table[i].data_type == 0x0004) // "special mode" UTF-8 (not null terminated)
    {
      Log_ErrorPrintf("Unhandled special mode UTF-8 type found in SFO table for entry %zu", i);
      return false;
    }
    else if (m_sfo_index_table[i].data_type == 0x0204) // null-terminated UTF-8 character string
    {
      std::vector<char> data_cstr(m_sfo_index_table[i].data_size);
      if (fgets(data_cstr.data(), static_cast<int>(data_cstr.size() * sizeof(char)), m_file) == nullptr)
      {
        Log_ErrorPrintf("Failed to read data string for SFO table entry %zu", i);
        return false;
      }

      m_sfo_table.emplace(std::string(key_cstr), std::string(data_cstr.data()));
    }
    else if (m_sfo_index_table[i].data_type == 0x0404) // uint32_t
    {
      u32 val;
      if (fread(&val, sizeof(u32), 1, m_file) != 1)
      {
        Log_ErrorPrintf("Failed to read unsigned data value for SFO table entry %zu", i);
        return false;
      }

      m_sfo_table.emplace(std::string(key_cstr), val);
    }
    else
    {
      Log_ErrorPrintf("Unhandled SFO data type 0x%04X found in SFO table for entry %zu", m_sfo_index_table[i].data_type,
                      i);
      return false;
    }
  }

#if _DEBUG
  PrintSFOTable(m_sfo_table);
#endif

  return true;
}

bool CDImagePBP::IsValidEboot(Common::Error* error)
{
  // Check some fields to make sure this is a valid PS1 EBOOT.PBP

  auto a_it = m_sfo_table.find("BOOTABLE");
  if (a_it != m_sfo_table.end())
  {
    SFOTableDataValue data_value = a_it->second;
    if (!std::holds_alternative<u32>(data_value) || std::get<u32>(data_value) != 1)
    {
      Log_ErrorPrint("Invalid BOOTABLE value");
      if (error)
        error->SetMessage("Invalid BOOTABLE value");
      return false;
    }
  }
  else
  {
    Log_ErrorPrint("No BOOTABLE value found");
    if (error)
      error->SetMessage("No BOOTABLE value found");
    return false;
  }

  a_it = m_sfo_table.find("CATEGORY");
  if (a_it != m_sfo_table.end())
  {
    SFOTableDataValue data_value = a_it->second;
    if (!std::holds_alternative<std::string>(data_value) || std::get<std::string>(data_value) != "ME")
    {
      Log_ErrorPrint("Invalid CATEGORY value");
      if (error)
        error->SetMessage("Invalid CATEGORY value");
      return false;
    }
  }
  else
  {
    Log_ErrorPrint("No CATEGORY value found");
    if (error)
      error->SetMessage("No CATEGORY value found");
    return false;
  }

  return true;
}

bool CDImagePBP::Open(const char* filename, Common::Error* error)
{
  if (!EndianHelper::HostIsLittleEndian())
  {
    Log_ErrorPrint("Big endian hosts not currently supported");
    return false;
  }

  m_file = FileSystem::OpenCFile(filename, "rb");
  if (!m_file)
  {
    if (error)
      error->SetErrno(errno);

    return false;
  }

  m_filename = filename;

  // Read in PBP header
  if (!LoadPBPHeader())
  {
    Log_ErrorPrint("Failed to load PBP header");
    if (error)
      error->SetMessage("Failed to load PBP header");
    return false;
  }

  // Read in SFO header
  if (!LoadSFOHeader())
  {
    Log_ErrorPrint("Failed to load SFO header");
    if (error)
      error->SetMessage("Failed to load SFO header");
    return false;
  }

  // Read in SFO index table
  if (!LoadSFOIndexTable())
  {
    Log_ErrorPrint("Failed to load SFO index table");
    if (error)
      error->SetMessage("Failed to load SFO index table");
    return false;
  }

  // Read in SFO table
  if (!LoadSFOTable())
  {
    Log_ErrorPrint("Failed to load SFO table");
    if (error)
      error->SetMessage("Failed to load SFO table");
    return false;
  }

  // Since PBP files can store things that aren't PS1 CD images, make sure we're loading the right kind
  if (!IsValidEboot(error))
  {
    Log_ErrorPrint("Couldn't validate EBOOT");
    return false;
  }

  // Start parsing ISO stuff
  if (FSeek64(m_file, m_pbp_header.data_psar_offset, SEEK_SET) != 0)
    return false;

  // Check "PSTITLEIMG000000" for multi-disc
  char data_psar_magic[16] = {};
  if (fread(data_psar_magic, sizeof(data_psar_magic), 1, m_file) != 1)
    return false;

  if (strncmp(data_psar_magic, "PSTITLEIMG000000", 16) == 0) // Multi-disc header found
  {
    // For multi-disc, the five disc offsets are located at data_psar_offset + 0x200. Non-present discs have an offset
    // of 0. There are also some disc hashes, a serial (from one of the discs, but used as an identifier for the entire
    // "title image" header), and some other offsets, but we don't really need to check those

    if (FSeek64(m_file, m_pbp_header.data_psar_offset + 0x200, SEEK_SET) != 0)
      return false;

    u32 disc_table[DISC_TABLE_NUM_ENTRIES] = {};
    if (fread(disc_table, sizeof(u32), DISC_TABLE_NUM_ENTRIES, m_file) != DISC_TABLE_NUM_ENTRIES)
      return false;

    // Ignore encrypted files
    if (disc_table[0] == 0x44475000) // "\0PGD"
    {
      Log_ErrorPrintf("Encrypted PBP images are not supported, skipping %s", m_filename.c_str());
      if (error)
        error->SetMessage("Encrypted PBP images are not supported");

      return false;
    }

    // Convert relative offsets to absolute offsets for available discs
    for (u32 i = 0; i < DISC_TABLE_NUM_ENTRIES; i++)
    {
      if (disc_table[i] != 0)
        m_disc_offsets.push_back(m_pbp_header.data_psar_offset + disc_table[i]);
      else
        break;
    }

    if (m_disc_offsets.size() < 1)
    {
      Log_ErrorPrintf("Invalid number of discs (%u) in multi-disc PBP file", static_cast<u32>(m_disc_offsets.size()));
      return false;
    }
  }
  else // Single-disc
  {
    m_disc_offsets.push_back(m_pbp_header.data_psar_offset);
  }

  // Default to first disc for now
  return OpenDisc(0, error);
}

bool CDImagePBP::OpenDisc(u32 index, Common::Error* error)
{
  if (index >= m_disc_offsets.size())
  {
    Log_ErrorPrintf("File does not contain disc %u", index + 1);
    if (error)
      error->SetMessage(TinyString::FromFormat("File does not contain disc %u", index + 1));
    return false;
  }

  m_current_block = static_cast<u32>(-1);
  m_blockinfo_table.fill({});
  m_toc.fill({});
  m_decompressed_block.fill(0x00);
  m_compressed_block.clear();

  // Go to ISO header
  const u32 iso_header_start = m_disc_offsets[index];
  if (FSeek64(m_file, iso_header_start, SEEK_SET) != 0)
    return false;

  char iso_header_magic[12] = {};
  if (fread(iso_header_magic, sizeof(iso_header_magic), 1, m_file) != 1)
    return false;

  if (strncmp(iso_header_magic, "PSISOIMG0000", 12) != 0)
  {
    Log_ErrorPrint("ISO header magic number mismatch");
    return false;
  }

  // Ignore encrypted files
  u32 pgd_magic;
  if (FSeek64(m_file, iso_header_start + 0x400, SEEK_SET) != 0)
    return false;

  if (fread(&pgd_magic, sizeof(pgd_magic), 1, m_file) != 1)
    return false;

  if (pgd_magic == 0x44475000) // "\0PGD"
  {
    Log_ErrorPrintf("Encrypted PBP images are not supported, skipping %s", m_filename.c_str());
    if (error)
      error->SetMessage("Encrypted PBP images are not supported");

    return false;
  }

  // Read in the TOC
  if (FSeek64(m_file, iso_header_start + 0x800, SEEK_SET) != 0)
    return false;

  for (u32 i = 0; i < TOC_NUM_ENTRIES; i++)
  {
    if (fread(&m_toc[i], sizeof(m_toc[i]), 1, m_file) != 1)
      return false;
  }

  // For homebrew EBOOTs, audio track table doesn't exist -- the data track block table will point to compressed blocks
  // for both data and audio

  // Get the offset of the compressed iso
  if (FSeek64(m_file, iso_header_start + 0xBFC, SEEK_SET) != 0)
    return false;

  u32 iso_offset;
  if (fread(&iso_offset, sizeof(iso_offset), 1, m_file) != 1)
    return false;

  // Generate block info table
  if (FSeek64(m_file, iso_header_start + 0x4000, SEEK_SET) != 0)
    return false;

  for (u32 i = 0; i < BLOCK_TABLE_NUM_ENTRIES; i++)
  {
    BlockTableEntry bte;
    if (fread(&bte, sizeof(bte), 1, m_file) != 1)
      return false;

    // Only store absolute file offset into a BlockInfo if this is a valid block
    m_blockinfo_table[i] = {(bte.size != 0) ? (iso_header_start + iso_offset + bte.offset) : 0, bte.size};

    // printf("Block %u, file offset %u, size %u\n", i, m_blockinfo_table[i].offset, m_blockinfo_table[i].size);
  }

  // iso_header_start + 0x12D4, 0x12D6, 0x12D8 supposedly contain data on block size, num clusters, and num blocks
  // Might be useful for error checking, but probably not that important as of now

  // Ignore track types for first three TOC entries, these don't seem to be consistent, but check that the points are
  // valid. Not sure what m_toc[0].userdata_start.s encodes on homebrew EBOOTs though, so ignore that
  if (m_toc[0].point != 0xA0 || m_toc[1].point != 0xA1 || m_toc[2].point != 0xA2)
  {
    Log_ErrorPrint("Invalid points on information tracks");
    return false;
  }

  const u8 first_track = PackedBCDToBinary(m_toc[0].userdata_start.m);
  const u8 last_track = PackedBCDToBinary(m_toc[1].userdata_start.m);
  const LBA sectors_on_file =
    Position::FromBCD(m_toc[2].userdata_start.m, m_toc[2].userdata_start.s, m_toc[2].userdata_start.f).ToLBA();

  if (first_track != 1 || last_track < first_track)
  {
    Log_ErrorPrint("Invalid starting track number or track count");
    return false;
  }

  // We assume that the pregap for the data track (track 1) is not on file, but pregaps for any additional tracks are on
  // file. Also, homebrew tools seem to create 2 second pregaps for audio tracks, even when the audio track has a pregap
  // that isn't 2 seconds long. We don't have a good way to validate this, and have to assume the TOC is giving us
  // correct pregap lengths...

  ClearTOC();
  m_lba_count = sectors_on_file;
  LBA track1_pregap_frames = 0;
  for (u32 curr_track = 1; curr_track <= last_track; curr_track++)
  {
    // Load in all the user stuff to m_tracks and m_indices
    const TOCEntry& t = m_toc[static_cast<size_t>(curr_track) + 2];
    const u8 track_num = PackedBCDToBinary(t.point);
    if (track_num != curr_track)
      Log_WarningPrintf("Mismatched TOC track number, expected %u but got %u", static_cast<u32>(curr_track), track_num);

    const bool is_audio_track = t.type == 0x01;
    const bool is_first_track = curr_track == 1;
    const bool is_last_track = curr_track == last_track;
    const TrackMode track_mode = is_audio_track ? TrackMode::Audio : TrackMode::Mode2Raw;
    const u32 track_sector_size = GetBytesPerSector(track_mode);

    SubChannelQ::Control track_control = {};
    track_control.data = !is_audio_track;

    LBA pregap_start = Position::FromBCD(t.pregap_start.m, t.pregap_start.s, t.pregap_start.f).ToLBA();
    LBA userdata_start = Position::FromBCD(t.userdata_start.m, t.userdata_start.s, t.userdata_start.f).ToLBA();
    LBA pregap_frames;
    u32 pregap_sector_size;

    if (userdata_start < pregap_start)
    {
      if (!is_first_track || is_audio_track)
      {
        Log_ErrorPrintf("Invalid TOC entry at index %u, user data (%u) should not start before pregap (%u)",
                        static_cast<u32>(curr_track), userdata_start, pregap_start);
        return false;
      }

      Log_WarningPrintf(
        "Invalid TOC entry at index %u, user data (%u) should not start before pregap (%u), assuming not in file.",
        static_cast<u32>(curr_track), userdata_start, pregap_start);
      pregap_start = 0;
      pregap_frames = userdata_start;
      pregap_sector_size = 0;
    }
    else
    {
      pregap_frames = userdata_start - pregap_start;
      pregap_sector_size = track_sector_size;
    }

    if (is_first_track)
    {
      m_lba_count += pregap_frames;
      track1_pregap_frames = pregap_frames;
    }

    Index pregap_index = {};
    pregap_index.file_offset =
      is_first_track ? 0 : (static_cast<u64>(pregap_start - track1_pregap_frames) * pregap_sector_size);
    pregap_index.file_index = 0;
    pregap_index.file_sector_size = pregap_sector_size;
    pregap_index.start_lba_on_disc = pregap_start;
    pregap_index.track_number = curr_track;
    pregap_index.index_number = 0;
    pregap_index.start_lba_in_track = static_cast<LBA>(-static_cast<s32>(pregap_frames));
    pregap_index.length = pregap_frames;
    pregap_index.mode = track_mode;
    pregap_index.control.bits = track_control.bits;
    pregap_index.is_pregap = true;

    m_indices.push_back(pregap_index);

    Index userdata_index = {};
    userdata_index.file_offset = static_cast<u64>(userdata_start - track1_pregap_frames) * track_sector_size;
    userdata_index.file_index = 0;
    userdata_index.file_sector_size = track_sector_size;
    userdata_index.start_lba_on_disc = userdata_start;
    userdata_index.track_number = curr_track;
    userdata_index.index_number = 1;
    userdata_index.start_lba_in_track = 0;
    userdata_index.mode = track_mode;
    userdata_index.control.bits = track_control.bits;
    userdata_index.is_pregap = false;

    if (is_last_track)
    {
      if (userdata_start >= m_lba_count)
      {
        Log_ErrorPrintf("Last user data index on disc for TOC entry %u should not be 0 or less in length",
                        static_cast<u32>(curr_track));
        return false;
      }
      userdata_index.length = m_lba_count - userdata_start;
    }
    else
    {
      const TOCEntry& next_track = m_toc[static_cast<size_t>(curr_track) + 3];
      const LBA next_track_start =
        Position::FromBCD(next_track.pregap_start.m, next_track.pregap_start.s, next_track.pregap_start.f).ToLBA();
      const u8 next_track_num = PackedBCDToBinary(next_track.point);

      if (next_track_num != curr_track + 1 || next_track_start < userdata_start)
      {
        Log_ErrorPrintf("Unable to calculate user data index length for TOC entry %u", static_cast<u32>(curr_track));
        return false;
      }

      userdata_index.length = next_track_start - userdata_start;
    }

    m_indices.push_back(userdata_index);

    m_tracks.push_back(Track{curr_track, userdata_start, 2 * curr_track - 1,
                             pregap_index.length + userdata_index.length, track_mode, track_control});
  }

  AddLeadOutIndex();

  // Initialize zlib stream
  if (!InitDecompressionStream())
  {
    Log_ErrorPrint("Failed to initialize zlib decompression stream");
    return false;
  }

  if (m_disc_offsets.size() > 1)
  {
    std::string sbi_path(Path::StripExtension(m_filename));
    sbi_path += TinyString::FromFormat("_%u.sbi", index + 1);
    m_sbi.LoadSBI(sbi_path.c_str());
  }
  else
    m_sbi.LoadSBI(Path::ReplaceExtension(m_filename, "sbi").c_str());

  m_current_disc = index;
  return Seek(1, Position{0, 0, 0});
}

const std::string* CDImagePBP::LookupStringSFOTableEntry(const char* key, const SFOTable& table)
{
  auto iter = table.find(key);
  if (iter == table.end())
    return nullptr;

  const SFOTableDataValue& data_value = iter->second;
  if (!std::holds_alternative<std::string>(data_value))
    return nullptr;

  return &std::get<std::string>(data_value);
}

bool CDImagePBP::InitDecompressionStream()
{
  m_inflate_stream = {};
  m_inflate_stream.next_in = Z_NULL;
  m_inflate_stream.avail_in = 0;
  m_inflate_stream.zalloc = Z_NULL;
  m_inflate_stream.zfree = Z_NULL;
  m_inflate_stream.opaque = Z_NULL;

  int ret = inflateInit2(&m_inflate_stream, -MAX_WBITS);
  return ret == Z_OK;
}

bool CDImagePBP::DecompressBlock(const BlockInfo& block_info)
{
  if (FSeek64(m_file, block_info.offset, SEEK_SET) != 0)
    return false;

  // Compression level 0 has compressed size == decompressed size.
  if (block_info.size == m_decompressed_block.size())
  {
    return (fread(m_decompressed_block.data(), sizeof(u8), m_decompressed_block.size(), m_file) ==
            m_decompressed_block.size());
  }

  m_compressed_block.resize(block_info.size);

  if (fread(m_compressed_block.data(), sizeof(u8), m_compressed_block.size(), m_file) != m_compressed_block.size())
    return false;

  m_inflate_stream.next_in = m_compressed_block.data();
  m_inflate_stream.avail_in = static_cast<uInt>(m_compressed_block.size());
  m_inflate_stream.next_out = m_decompressed_block.data();
  m_inflate_stream.avail_out = static_cast<uInt>(m_decompressed_block.size());

  if (inflateReset(&m_inflate_stream) != Z_OK)
    return false;

  int err = inflate(&m_inflate_stream, Z_FINISH);
  if (err != Z_STREAM_END)
  {
    Log_ErrorPrintf("Inflate error %d", err);
    return false;
  }

  return true;
}

bool CDImagePBP::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  if (m_sbi.GetReplacementSubChannelQ(index.start_lba_on_disc + lba_in_index, subq))
    return true;

  return CDImage::ReadSubChannelQ(subq, index, lba_in_index);
}

bool CDImagePBP::HasNonStandardSubchannel() const
{
  return (m_sbi.GetReplacementSectorCount() > 0);
}

bool CDImagePBP::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  const u32 offset_in_file = static_cast<u32>(index.file_offset) + (lba_in_index * index.file_sector_size);
  const u32 offset_in_block = offset_in_file % DECOMPRESSED_BLOCK_SIZE;
  const u32 requested_block = offset_in_file / DECOMPRESSED_BLOCK_SIZE;

  BlockInfo& bi = m_blockinfo_table[requested_block];

  if (bi.size == 0)
  {
    Log_ErrorPrintf("Invalid block %u requested", requested_block);
    return false;
  }

  if (m_current_block != requested_block && !DecompressBlock(bi))
  {
    Log_ErrorPrintf("Failed to decompress block %u", requested_block);
    return false;
  }

  std::memcpy(buffer, &m_decompressed_block[offset_in_block], RAW_SECTOR_SIZE);
  return true;
}

#if _DEBUG
void CDImagePBP::PrintPBPHeaderInfo(const PBPHeader& pbp_header)
{
  printf("PBP header info\n");
  printf("PBP format version 0x%08X\n", pbp_header.version);
  printf("File offsets\n");
  printf("PARAM.SFO 0x%08X PARSE\n", pbp_header.param_sfo_offset);
  printf("ICON0.PNG 0x%08X IGNORE\n", pbp_header.icon0_png_offset);
  printf("ICON1.PNG 0x%08X IGNORE\n", pbp_header.icon1_png_offset);
  printf("PIC0.PNG  0x%08X IGNORE\n", pbp_header.pic0_png_offset);
  printf("PIC1.PNG  0x%08X IGNORE\n", pbp_header.pic1_png_offset);
  printf("SND0.AT3  0x%08X IGNORE\n", pbp_header.snd0_at3_offset);
  printf("DATA.PSP  0x%08X IGNORE\n", pbp_header.data_psp_offset);
  printf("DATA.PSAR 0x%08X PARSE\n", pbp_header.data_psar_offset);
  printf("\n");
}

void CDImagePBP::PrintSFOHeaderInfo(const SFOHeader& sfo_header)
{
  printf("SFO header info\n");
  printf("SFO format version    0x%08X\n", sfo_header.version);
  printf("SFO key table offset  0x%08X\n", sfo_header.key_table_offset);
  printf("SFO data table offset 0x%08X\n", sfo_header.data_table_offset);
  printf("SFO table entry count 0x%08X\n", sfo_header.num_table_entries);
  printf("\n");
}

void CDImagePBP::PrintSFOIndexTableEntry(const SFOIndexTableEntry& sfo_index_table_entry, size_t i)
{
  printf("SFO index table entry %zu\n", i);
  printf("Key offset      0x%08X\n", sfo_index_table_entry.key_offset);
  printf("Data type       0x%08X\n", sfo_index_table_entry.data_type);
  printf("Data size       0x%08X\n", sfo_index_table_entry.data_size);
  printf("Total data size 0x%08X\n", sfo_index_table_entry.data_total_size);
  printf("Data offset     0x%08X\n", sfo_index_table_entry.data_offset);
  printf("\n");
}

void CDImagePBP::PrintSFOTable(const SFOTable& sfo_table)
{
  for (auto it = sfo_table.begin(); it != sfo_table.end(); ++it)
  {
    std::string key_value = it->first;
    SFOTableDataValue data_value = it->second;

    if (std::holds_alternative<std::string>(data_value))
      printf("Key: %s, Data: %s\n", key_value.c_str(), std::get<std::string>(data_value).c_str());
    else if (std::holds_alternative<u32>(data_value))
      printf("Key: %s, Data: %u\n", key_value.c_str(), std::get<u32>(data_value));
  }
}
#endif

bool CDImagePBP::HasSubImages() const
{
  return m_disc_offsets.size() > 1;
}

std::string CDImagePBP::GetMetadata(const std::string_view& type) const
{
  if (type == "title")
  {
    const std::string* title = LookupStringSFOTableEntry("TITLE", m_sfo_table);
    if (title && !title->empty())
      return *title;
  }

  return CDImage::GetMetadata(type);
}

u32 CDImagePBP::GetSubImageCount() const
{
  return static_cast<u32>(m_disc_offsets.size());
}

u32 CDImagePBP::GetCurrentSubImage() const
{
  return m_current_disc;
}

bool CDImagePBP::SwitchSubImage(u32 index, Common::Error* error)
{
  if (index >= m_disc_offsets.size())
    return false;

  const u32 old_disc = m_current_disc;
  if (!OpenDisc(index, error))
  {
    // return to old disc, this should never fail... in theory.
    if (!OpenDisc(old_disc, nullptr))
      Panic("Failed to reopen old disc after switch.");
  }

  return true;
}

std::string CDImagePBP::GetSubImageMetadata(u32 index, const std::string_view& type) const
{
  if (type == "title")
  {
    const std::string* title = LookupStringSFOTableEntry("TITLE", m_sfo_table);
    if (title && !title->empty())
      return StringUtil::StdStringFromFormat("%s (Disc %u)", title->c_str(), index + 1);
  }

  return CDImage::GetSubImageMetadata(index, type);
}

std::unique_ptr<CDImage> CDImage::OpenPBPImage(const char* filename, Common::Error* error)
{
  std::unique_ptr<CDImagePBP> image = std::make_unique<CDImagePBP>();
  if (!image->Open(filename, error))
    return {};

  return image;
}
