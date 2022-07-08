#include "cd_image.h"
#include "cd_subchannel_replacement.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include <algorithm>
#include <cerrno>
#include <map>
#include <sstream>
Log_SetChannel(CDImageMemory);

class CDImageM3u : public CDImage
{
public:
  CDImageM3u();
  ~CDImageM3u() override;

  bool Open(const char* path, Common::Error* Error);

  bool ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index) override;
  bool HasNonStandardSubchannel() const override;

  bool HasSubImages() const override;
  u32 GetSubImageCount() const override;
  u32 GetCurrentSubImage() const override;
  std::string GetSubImageMetadata(u32 index, const std::string_view& type) const override;
  bool SwitchSubImage(u32 index, Common::Error* error) override;

protected:
  bool ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index) override;

private:
  struct Entry
  {
    // TODO: Worth storing any other data?
    std::string filename;
    std::string title;
  };

  std::vector<Entry> m_entries;
  std::unique_ptr<CDImage> m_current_image;
  u32 m_current_image_index = UINT32_C(0xFFFFFFFF);
};

CDImageM3u::CDImageM3u() = default;

CDImageM3u::~CDImageM3u() = default;

bool CDImageM3u::Open(const char* path, Common::Error* error)
{
  std::FILE* fp = FileSystem::OpenCFile(path, "rb");
  if (!fp)
    return false;

  std::optional<std::string> m3u_file(FileSystem::ReadFileToString(fp));
  std::fclose(fp);
  if (!m3u_file.has_value() || m3u_file->empty())
  {
    if (error)
      error->SetMessage("Failed to read M3u file");
    return false;
  }

  std::istringstream ifs(m3u_file.value());
  m_filename = path;

  std::vector<std::string> entries;
  std::string line;
  while (std::getline(ifs, line))
  {
    u32 start_offset = 0;
    while (start_offset < line.size() && std::isspace(line[start_offset]))
      start_offset++;

    // skip comments
    if (start_offset == line.size() || line[start_offset] == '#')
      continue;

    // strip ending whitespace
    u32 end_offset = static_cast<u32>(line.size()) - 1;
    while (std::isspace(line[end_offset]) && end_offset > start_offset)
      end_offset--;

    // anything?
    if (start_offset == end_offset)
      continue;

    Entry entry;
    std::string entry_filename(line.begin() + start_offset, line.begin() + end_offset + 1);
    entry.title = Path::GetFileTitle(entry_filename);
    if (!Path::IsAbsolute(entry_filename))
      entry.filename = Path::BuildRelativePath(path, entry_filename);
    else
      entry.filename = std::move(entry_filename);

    Log_DevPrintf("Read path from m3u: '%s'", entry.filename.c_str());
    m_entries.push_back(std::move(entry));
  }

  Log_InfoPrintf("Loaded %zu paths from m3u '%s'", m_entries.size(), path);
  return !m_entries.empty() && SwitchSubImage(0, error);
}

bool CDImageM3u::HasNonStandardSubchannel() const
{
  return m_current_image->HasNonStandardSubchannel();
}

bool CDImageM3u::HasSubImages() const
{
  return true;
}

u32 CDImageM3u::GetSubImageCount() const
{
  return static_cast<u32>(m_entries.size());
}

u32 CDImageM3u::GetCurrentSubImage() const
{
  return m_current_image_index;
}

bool CDImageM3u::SwitchSubImage(u32 index, Common::Error* error)
{
  if (index >= m_entries.size())
    return false;
  else if (index == m_current_image_index)
    return true;

  const Entry& entry = m_entries[index];
  std::unique_ptr<CDImage> new_image = CDImage::Open(entry.filename.c_str(), error);
  if (!new_image)
  {
    Log_ErrorPrintf("Failed to load subimage %u (%s)", index, entry.filename.c_str());
    return false;
  }

  CopyTOC(new_image.get());
  m_current_image = std::move(new_image);
  m_current_image_index = index;
  if (!Seek(1, Position{0, 0, 0}))
    Panic("Failed to seek to start after sub-image change.");

  return true;
}

std::string CDImageM3u::GetSubImageMetadata(u32 index, const std::string_view& type) const
{
  if (index > m_entries.size())
    return {};

  if (type == "title")
    return m_entries[index].title;
  else if (type == "file_title")
    return std::string(Path::GetFileTitle(m_entries[index].filename));

  return CDImage::GetSubImageMetadata(index, type);
}

bool CDImageM3u::ReadSectorFromIndex(void* buffer, const Index& index, LBA lba_in_index)
{
  return m_current_image->ReadSectorFromIndex(buffer, index, lba_in_index);
}

bool CDImageM3u::ReadSubChannelQ(SubChannelQ* subq, const Index& index, LBA lba_in_index)
{
  return m_current_image->ReadSubChannelQ(subq, index, lba_in_index);
}

std::unique_ptr<CDImage> CDImage::OpenM3uImage(const char* filename, Common::Error* error)
{
  std::unique_ptr<CDImageM3u> image = std::make_unique<CDImageM3u>();
  if (!image->Open(filename, error))
    return {};

  return image;
}
