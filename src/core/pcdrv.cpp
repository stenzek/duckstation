// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "pcdrv.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "cpu_core.h"
#include "settings.h"
Log_SetChannel(PCDrv);

static constexpr u32 MAX_FILES = 100;

static std::vector<FileSystem::ManagedCFilePtr> s_files;

enum PCDrvAttribute : u32
{
  PCDRV_ATTRIBUTE_READ_ONLY = (1 << 0),
  PCDRV_ATTRIBUTE_HIDDEN = (1 << 1),
  PCDRV_ATTRIBUTE_SYSTEM = (1 << 2),
  PCDRV_ATTRIBUTE_DIRECTORY = (1 << 4),
  PCDRV_ATTRIBUTE_ARCHIVE = (1 << 5),
};

static s32 GetFreeFileHandle()
{
  for (s32 i = 0; i < static_cast<s32>(s_files.size()); i++)
  {
    if (!s_files[i])
      return i;
  }

  if (s_files.size() >= MAX_FILES)
  {
    Log_ErrorPrint("Too many open files.");
    return -1;
  }

  const s32 index = static_cast<s32>(s_files.size());
  s_files.emplace_back(nullptr, FileSystem::FileDeleter());
  return index;
}

static void CloseAllFiles()
{
  if (!s_files.empty())
    Log_DevPrintf("Closing %zu open files.", s_files.size());

  s_files.clear();
}

static FILE* GetFileFromHandle(u32 handle)
{
  if (handle >= static_cast<u32>(s_files.size()) || !s_files[handle])
  {
    Log_ErrorPrintf("Invalid file handle %d", static_cast<s32>(handle));
    return nullptr;
  }

  return s_files[handle].get();
}

static bool CloseFileHandle(u32 handle)
{
  if (handle >= static_cast<u32>(s_files.size()) || !s_files[handle])
  {
    Log_ErrorPrintf("Invalid file handle %d", static_cast<s32>(handle));
    return false;
  }

  s_files[handle].reset();
  while (!s_files.empty() && !s_files.back())
    s_files.pop_back();
  return true;
}

static std::string ResolveHostPath(const std::string& path)
{
  // Double-check that it falls within the directory of the elf.
  // Not a real sandbox, but emulators shouldn't be treated as such. Don't run untrusted code!
  const std::string& root = g_settings.pcdrv_root;
  std::string canonicalized_path = Path::Canonicalize(Path::Combine(root, path));
  if (canonicalized_path.length() < root.length() ||                      // Length has to be longer (a file),
      !canonicalized_path.starts_with(root) ||                            // and start with the host root,
      canonicalized_path[root.length()] != FS_OSPATH_SEPARATOR_CHARACTER) // and we can't access a sibling.
  {
    Log_ErrorPrintf("Denying access to path outside of PCDrv directory. Requested path: '%s', "
                    "Resolved path: '%s', Root directory: '%s'",
                    path.c_str(), root.c_str(), canonicalized_path.c_str());
    canonicalized_path.clear();
  }

  return canonicalized_path;
}

void PCDrv::Initialize()
{
  if (!g_settings.pcdrv_enable)
    return;

  Log_WarningPrintf("%s PCDrv is enabled at '%s'", g_settings.pcdrv_enable_writes ? "Read/Write" : "Read-Only",
                    g_settings.pcdrv_root.c_str());
}

void PCDrv::Reset()
{
  CloseAllFiles();
}

void PCDrv::Shutdown()
{
  CloseAllFiles();
}

bool PCDrv::HandleSyscall(u32 instruction_bits, CPU::Registers& regs)
{
  // Based on https://problemkaputt.de/psxspx-bios-pc-file-server.htm

#define RETURN_ERROR()                                                                                                 \
  regs.v0 = 0xffffffff;                                                                                                \
  regs.v1 = 0xffffffff; // error code

  const u32 code = (instruction_bits >> 6) & 0xfffff; // 20 bits, funct = 0
  switch (code)
  {
    case 0x101: // PCinit
    {
      Log_DevPrintf("PCinit");
      CloseAllFiles();
      regs.v0 = 0;
      regs.v1 = 0;
      return true;
    }

    case 0x102: // PCcreat
    case 0x103: // PCopen
    {
      const bool is_open = (code == 0x103);
      const char* func = (code == 0x102) ? "PCcreat" : "PCopen";
      const u32 mode = regs.a2;
      std::string filename;
      if (!CPU::SafeReadMemoryCString(regs.a1, &filename))
      {
        Log_ErrorPrintf("%s: Invalid string", func);
        return false;
      }

      Log_DebugPrintf("%s: '%s' mode %u", func, filename.c_str(), mode);
      if ((filename = ResolveHostPath(filename)).empty())
      {
        RETURN_ERROR();
        return true;
      }

      if (!is_open && !g_settings.pcdrv_enable_writes)
      {
        Log_ErrorPrintf("%s: Writes are not enabled", func);
        RETURN_ERROR();
        return true;
      }

      // Directories are unsupported for now, ignore other attributes
      if (mode & PCDRV_ATTRIBUTE_DIRECTORY)
      {
        Log_ErrorPrintf("%s: Directories are unsupported", func);
        RETURN_ERROR();
        return true;
      }

      // Create empty file, truncate if exists.
      const s32 handle = GetFreeFileHandle();
      if (handle < 0)
      {
        RETURN_ERROR();
        return true;
      }

      s_files[handle] = FileSystem::OpenManagedCFile(filename.c_str(),
                                                     is_open ? (g_settings.pcdrv_enable_writes ? "r+b" : "rb") : "w+b");
      if (!s_files[handle])
      {
        Log_ErrorPrintf("%s: Failed to open '%s'", func, filename.c_str());
        RETURN_ERROR();
        return true;
      }

      Log_DebugPrintf("PCDrv: Opened '%s' => %d", filename.c_str(), handle);
      regs.v0 = 0;
      regs.v1 = static_cast<u32>(handle);
      return true;
    }

    case 0x104: // PCclose
    {
      Log_DebugPrintf("PCclose(%u)", regs.a1);

      if (!CloseFileHandle(regs.a1))
      {
        RETURN_ERROR();
        return true;
      }

      regs.v0 = 0;
      regs.v1 = 0;
      return true;
    }

    case 0x105: // PCread
    {
      Log_DebugPrintf("PCread(%u, %u, 0x%08x)", regs.a1, regs.a2, regs.a3);

      std::FILE* fp = GetFileFromHandle(regs.a1);
      if (!fp)
      {
        RETURN_ERROR();
        return true;
      }

      const u32 count = regs.a2;
      u32 dstaddr = regs.a3;
      for (u32 i = 0; i < count; i++)
      {
        // Certainly less than optimal, but it's not like you're going to be reading megabytes of data here.
        u8 val;
        if (std::fread(&val, 1, 1, fp) != 1)
        {
          // Does not stop at EOF according to psx-spx.
          if (std::ferror(fp) != 0)
          {
            RETURN_ERROR();
            return true;
          }

          val = 0;
        }

        CPU::SafeWriteMemoryByte(dstaddr, val);
        dstaddr++;
      }

      regs.v0 = 0;
      regs.v1 = count;
      return true;
    }

    case 0x106: // PCwrite
    {
      Log_DebugPrintf("PCwrite(%u, %u, 0x%08x)", regs.a1, regs.a2, regs.a3);

      std::FILE* fp = GetFileFromHandle(regs.a1);
      if (!fp)
      {
        RETURN_ERROR();
        return true;
      }

      const u32 count = regs.a2;
      u32 srcaddr = regs.a3;
      u32 written = 0;
      for (u32 i = 0; i < count; i++)
      {
        u8 val;
        if (!CPU::SafeReadMemoryByte(srcaddr, &val))
          break;

        if (std::fwrite(&val, 1, 1, fp) != 1)
        {
          RETURN_ERROR();
          return true;
        }

        srcaddr++;
        written++;
      }

      regs.v0 = 0;
      regs.v1 = written;
      return true;
    }

    case 0x107: // PClseek
    {
      Log_DebugPrintf("PClseek(%u, %u, %u)", regs.a1, regs.a2, regs.a3);

      std::FILE* fp = GetFileFromHandle(regs.a1);
      if (!fp)
      {
        RETURN_ERROR();
        return true;
      }

      const s32 offset = static_cast<s32>(regs.a2);
      const u32 mode = regs.a3;
      int hmode;
      switch (mode)
      {
        case 0:
          hmode = SEEK_SET;
          break;
        case 1:
          hmode = SEEK_CUR;
          break;
        case 2:
          hmode = SEEK_END;
          break;
        default:
          RETURN_ERROR();
          return true;
      }

      if (FileSystem::FSeek64(fp, offset, hmode) != 0)
      {
        Log_ErrorPrintf("FSeek for PCDrv failed: %d %u", offset, hmode);
        RETURN_ERROR();
        return true;
      }

      regs.v0 = 0;
      regs.v1 = static_cast<u32>(static_cast<s32>(FileSystem::FTell64(fp)));
      return true;
    }

    default:
      return false;
  }
}
