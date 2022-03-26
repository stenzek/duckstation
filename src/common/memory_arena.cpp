#include "memory_arena.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
Log_SetChannel(Common::MemoryArena);

#if defined(_WIN32)
#include "common/windows_headers.h"
#elif defined(ANDROID)
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/ashmem.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Common {

// Borrowed from Dolphin
#ifdef ANDROID
#define ASHMEM_DEVICE "/dev/ashmem"

static int AshmemCreateFileMapping(const char* name, size_t size)
{
  // ASharedMemory path - works on API >= 26 and falls through on API < 26:

  // We can't call ASharedMemory_create the normal way without increasing the
  // minimum version requirement to API 26, so we use dlopen/dlsym instead
  static void* libandroid = dlopen("libandroid.so", RTLD_LAZY | RTLD_LOCAL);
  static auto shared_memory_create =
    reinterpret_cast<int (*)(const char*, size_t)>(dlsym(libandroid, "ASharedMemory_create"));
  if (shared_memory_create)
    return shared_memory_create(name, size);

  // /dev/ashmem path - works on API < 29:

  int fd, ret;
  fd = open(ASHMEM_DEVICE, O_RDWR);
  if (fd < 0)
    return fd;

  // We don't really care if we can't set the name, it is optional
  ioctl(fd, ASHMEM_SET_NAME, name);

  ret = ioctl(fd, ASHMEM_SET_SIZE, size);
  if (ret < 0)
  {
    close(fd);
    Log_ErrorPrintf("Ashmem returned error: 0x%08x", ret);
    return ret;
  }
  return fd;
}
#endif

MemoryArena::MemoryArena() = default;

MemoryArena::~MemoryArena()
{
  Destroy();
}

void* MemoryArena::FindBaseAddressForMapping(size_t size)
{
  void* base_address;
#if defined(_WIN32)
  base_address = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
  if (base_address)
    VirtualFree(base_address, 0, MEM_RELEASE);
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  base_address = mmap(nullptr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
  if (base_address)
    munmap(base_address, size);
#elif defined(__ANDROID__)
  base_address = mmap(nullptr, size, PROT_NONE, MAP_ANON | MAP_SHARED, -1, 0);
  if (base_address)
    munmap(base_address, size);
#else
  base_address = nullptr;
#endif

  if (!base_address)
  {
    Log_ErrorPrintf("Failed to get base address for memory mapping of size %zu", size);
    return nullptr;
  }

  return base_address;
}

bool MemoryArena::IsValid() const
{
#if defined(_WIN32)
  return m_file_handle != nullptr;
#else
  return m_shmem_fd >= 0;
#endif
}

static std::string GetFileMappingName()
{
#if defined(_WIN32)
  const unsigned pid = GetCurrentProcessId();
#elif defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  const unsigned pid = static_cast<unsigned>(getpid());
#else
#error Unknown platform.
#endif

  const std::string ret(StringUtil::StdStringFromFormat("duckstation_%u", pid));
  Log_InfoPrintf("File mapping name: %s", ret.c_str());
  return ret;
}

bool MemoryArena::Create(size_t size, bool writable, bool executable)
{
  if (IsValid())
    Destroy();

  const std::string file_mapping_name(GetFileMappingName());

#if defined(_WIN32)
  const DWORD protect = (writable ? (executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE) : PAGE_READONLY);
#ifndef _UWP
  m_file_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, protect, Truncate32(size >> 32), Truncate32(size),
                                     file_mapping_name.c_str());
#else
  m_file_handle = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, protect, size,
                                           StringUtil::UTF8StringToWideString(file_mapping_name).c_str());
#endif
  if (!m_file_handle)
  {
    Log_ErrorPrintf("CreateFileMapping failed: %u", GetLastError());
    return false;
  }

  m_size = size;
  m_writable = writable;
  m_executable = executable;
  return true;
#elif defined(__ANDROID__)
  m_shmem_fd = AshmemCreateFileMapping(file_mapping_name.c_str(), size);
  if (m_shmem_fd < 0)
  {
    Log_ErrorPrintf("AshmemCreateFileMapping failed: %d %d", m_shmem_fd, errno);
    return false;
  }

  m_size = size;
  m_writable = writable;
  m_executable = executable;
  return true;
#elif defined(__linux__)
  m_shmem_fd = shm_open(file_mapping_name.c_str(), O_CREAT | O_EXCL | (writable ? O_RDWR : O_RDONLY), 0600);
  if (m_shmem_fd < 0)
  {
    Log_ErrorPrintf("shm_open failed: %d", errno);
    return false;
  }

  // we're not going to be opening this mapping in other processes, so remove the file
  shm_unlink(file_mapping_name.c_str());

  // ensure it's the correct size
  if (ftruncate64(m_shmem_fd, static_cast<off64_t>(size)) < 0)
  {
    Log_ErrorPrintf("ftruncate64(%zu) failed: %d", size, errno);
    return false;
  }

  m_size = size;
  m_writable = writable;
  m_executable = executable;
  return true;
#elif defined(__APPLE__) || defined(__FreeBSD__)
#if defined(__APPLE__)
  m_shmem_fd = shm_open(file_mapping_name.c_str(), O_CREAT | O_EXCL | (writable ? O_RDWR : O_RDONLY), 0600);
#else
  m_shmem_fd = shm_open(SHM_ANON, O_CREAT | O_EXCL | (writable ? O_RDWR : O_RDONLY), 0600);
#endif

  if (m_shmem_fd < 0)
  {
    Log_ErrorPrintf("shm_open failed: %d", errno);
    return false;
  }

#ifdef __APPLE__
  // we're not going to be opening this mapping in other processes, so remove the file
  shm_unlink(file_mapping_name.c_str());
#endif

  // ensure it's the correct size
  if (ftruncate(m_shmem_fd, static_cast<off_t>(size)) < 0)
  {
    Log_ErrorPrintf("ftruncate(%zu) failed: %d", size, errno);
    return false;
  }

  m_size = size;
  m_writable = writable;
  m_executable = executable;
  return true;
#else
  return false;
#endif
}

void MemoryArena::Destroy()
{
#if defined(_WIN32)
  if (m_file_handle)
  {
    CloseHandle(m_file_handle);
    m_file_handle = nullptr;
  }
#elif defined(__linux__) || defined(__FreeBSD__)
  if (m_shmem_fd > 0)
  {
    close(m_shmem_fd);
    m_shmem_fd = -1;
  }
#endif
}

std::optional<MemoryArena::View> MemoryArena::CreateView(size_t offset, size_t size, bool writable, bool executable,
                                                         void* fixed_address)
{
  void* base_pointer = CreateViewPtr(offset, size, writable, executable, fixed_address);
  if (!base_pointer)
    return std::nullopt;

  return View(this, base_pointer, offset, size, writable);
}

std::optional<MemoryArena::View> MemoryArena::CreateReservedView(size_t size, void* fixed_address /*= nullptr*/)
{
  void* base_pointer = CreateReservedPtr(size, fixed_address);
  if (!base_pointer)
    return std::nullopt;

  return View(this, base_pointer, View::RESERVED_REGION_OFFSET, size, false);
}

void* MemoryArena::CreateViewPtr(size_t offset, size_t size, bool writable, bool executable,
                                 void* fixed_address /*= nullptr*/)
{
  void* base_pointer;
#if defined(_WIN32)
  const DWORD desired_access = FILE_MAP_READ | (writable ? FILE_MAP_WRITE : 0) | (executable ? FILE_MAP_EXECUTE : 0);
#ifndef _UWP
  base_pointer =
    MapViewOfFileEx(m_file_handle, desired_access, Truncate32(offset >> 32), Truncate32(offset), size, fixed_address);
#else
  // UWP does not support fixed mappings.
  if (!fixed_address)
    base_pointer = MapViewOfFileFromApp(m_file_handle, desired_access, offset, size);
  else
    base_pointer = nullptr;
#endif
  if (!base_pointer)
    return nullptr;
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  const int flags = (fixed_address != nullptr) ? (MAP_SHARED | MAP_FIXED) : MAP_SHARED;
  const int prot = PROT_READ | (writable ? PROT_WRITE : 0) | (executable ? PROT_EXEC : 0);
  base_pointer = mmap(fixed_address, size, prot, flags, m_shmem_fd, static_cast<off_t>(offset));
  if (base_pointer == reinterpret_cast<void*>(-1))
    return nullptr;
#else
  return nullptr;
#endif

  m_num_views.fetch_add(1);
  return base_pointer;
}

bool MemoryArena::FlushViewPtr(void* address, size_t size)
{
#if defined(_WIN32)
  return FlushViewOfFile(address, size);
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  return (msync(address, size, 0) >= 0);
#else
  return false;
#endif
}

bool MemoryArena::ReleaseViewPtr(void* address, size_t size)
{
  bool result;
#if defined(_WIN32)
  result = static_cast<bool>(UnmapViewOfFile(address));
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  result = (munmap(address, size) >= 0);
#else
  result = false;
#endif

  if (!result)
  {
    Log_ErrorPrintf("Failed to unmap previously-created view at %p", address);
    return false;
  }

  const size_t prev_count = m_num_views.fetch_sub(1);
  Assert(prev_count > 0);
  return true;
}

void* MemoryArena::CreateReservedPtr(size_t size, void* fixed_address /*= nullptr*/)
{
  void* base_pointer;
#if defined(_WIN32)
  base_pointer = VirtualAlloc(fixed_address, size, MEM_RESERVE, PAGE_NOACCESS);
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  const int flags =
    (fixed_address != nullptr) ? (MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED) : (MAP_PRIVATE | MAP_ANONYMOUS);
  base_pointer = mmap(fixed_address, size, PROT_NONE, flags, -1, 0);
  if (base_pointer == reinterpret_cast<void*>(-1))
    return nullptr;
#else
  return nullptr;
#endif

  m_num_views.fetch_add(1);
  return base_pointer;
}

bool MemoryArena::ReleaseReservedPtr(void* address, size_t size)
{
  bool result;
#if defined(_WIN32)
  result = static_cast<bool>(VirtualFree(address, 0, MEM_RELEASE));
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
  result = (munmap(address, size) >= 0);
#else
  result = false;
#endif

  if (!result)
  {
    Log_ErrorPrintf("Failed to release previously-created view at %p", address);
    return false;
  }

  const size_t prev_count = m_num_views.fetch_sub(1);
  Assert(prev_count > 0);
  return true;
}

bool MemoryArena::SetPageProtection(void* address, size_t length, bool readable, bool writable, bool executable)
{
#if defined(_WIN32)
  static constexpr DWORD protection_table[2][2][2] = {
    {{PAGE_NOACCESS, PAGE_EXECUTE}, {PAGE_WRITECOPY, PAGE_EXECUTE_WRITECOPY}},
    {{PAGE_READONLY, PAGE_EXECUTE_READ}, {PAGE_READWRITE, PAGE_EXECUTE_READWRITE}}};

  DWORD old_protect;
  return static_cast<bool>(
    VirtualProtect(address, length, protection_table[readable][writable][executable], &old_protect));
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__FreeBSD__)
  const int prot = (readable ? PROT_READ : 0) | (writable ? PROT_WRITE : 0) | (executable ? PROT_EXEC : 0);
  return (mprotect(address, length, prot) >= 0);
#else
  return false;
#endif
}

MemoryArena::View::View(MemoryArena* parent, void* base_pointer, size_t arena_offset, size_t mapping_size,
                        bool writable)
  : m_parent(parent), m_base_pointer(base_pointer), m_arena_offset(arena_offset), m_mapping_size(mapping_size),
    m_writable(writable)
{
}

MemoryArena::View::View(View&& view)
  : m_parent(view.m_parent), m_base_pointer(view.m_base_pointer), m_arena_offset(view.m_arena_offset),
    m_mapping_size(view.m_mapping_size)
{
  view.m_parent = nullptr;
  view.m_base_pointer = nullptr;
  view.m_arena_offset = 0;
  view.m_mapping_size = 0;
}

MemoryArena::View::~View()
{
  if (m_parent)
  {
    if (m_arena_offset != RESERVED_REGION_OFFSET)
    {
      if (m_writable && !m_parent->FlushViewPtr(m_base_pointer, m_mapping_size))
        Panic("Failed to flush previously-created view");
      if (!m_parent->ReleaseViewPtr(m_base_pointer, m_mapping_size))
        Panic("Failed to unmap previously-created view");
    }
    else
    {
      if (!m_parent->ReleaseReservedPtr(m_base_pointer, m_mapping_size))
        Panic("Failed to release previously-created view");
    }
  }
}
} // namespace Common
