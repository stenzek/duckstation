// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "memmap.h"
#include "align.h"
#include "assert.h"
#include "error.h"
#include "log.h"
#include "small_string.h"
#include "string_util.h"

#include "fmt/format.h"

#include <memory>

#if defined(_WIN32)
#include "windows_headers.h"
#include <Psapi.h>
#elif defined(__APPLE__)
#ifdef __aarch64__
#include <pthread.h> // pthread_jit_write_protect_np()
#endif
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#include <sys/mman.h>
#elif !defined(__ANDROID__)
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

Log_SetChannel(MemMap);

namespace MemMap {
/// Allocates RWX memory at the specified address.
static void* AllocateJITMemoryAt(const void* addr, size_t size);
} // namespace MemMap

#ifdef _WIN32

bool MemMap::MemProtect(void* baseaddr, size_t size, PageProtect mode)
{
  DebugAssert((size & (HOST_PAGE_SIZE - 1)) == 0);

  DWORD old_protect;
  if (!VirtualProtect(baseaddr, size, static_cast<DWORD>(mode), &old_protect))
  {
    ERROR_LOG("VirtualProtect() failed with error {}", GetLastError());
    return false;
  }

  return true;
}

std::string MemMap::GetFileMappingName(const char* prefix)
{
  const unsigned pid = GetCurrentProcessId();
  return fmt::format("{}_{}", prefix, pid);
}

void* MemMap::CreateSharedMemory(const char* name, size_t size, Error* error)
{
  const std::wstring mapping_name = name ? StringUtil::UTF8StringToWideString(name) : std::wstring();
  const HANDLE mapping =
    CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
                       static_cast<DWORD>(size), mapping_name.empty() ? nullptr : mapping_name.c_str());
  if (!mapping)
    Error::SetWin32(error, "CreateFileMappingW() failed: ", GetLastError());

  return static_cast<void*>(mapping);
}

void MemMap::DestroySharedMemory(void* ptr)
{
  CloseHandle(static_cast<HANDLE>(ptr));
}

void MemMap::DeleteSharedMemory(const char* name)
{
  // Automatically freed on close.
}

void* MemMap::MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, PageProtect mode)
{
  void* ret = MapViewOfFileEx(static_cast<HANDLE>(handle), FILE_MAP_READ | FILE_MAP_WRITE,
                              static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset), size, baseaddr);
  if (!ret)
    return nullptr;

  if (mode != PageProtect::ReadWrite)
  {
    DWORD old_prot;
    if (!VirtualProtect(ret, size, static_cast<DWORD>(mode), &old_prot))
      Panic("Failed to protect memory mapping");
  }
  return ret;
}

void MemMap::UnmapSharedMemory(void* baseaddr, size_t size)
{
  if (!UnmapViewOfFile(baseaddr))
    Panic("Failed to unmap shared memory");
}

const void* MemMap::GetBaseAddress()
{
  const HMODULE mod = GetModuleHandleW(nullptr);
  if (!mod)
    return nullptr;

  MODULEINFO mi;
  if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
    return mod;

  return mi.lpBaseOfDll;
}

void* MemMap::AllocateJITMemoryAt(const void* addr, size_t size)
{
  void* ptr = static_cast<u8*>(VirtualAlloc(const_cast<void*>(addr), size,
                                            addr ? (MEM_RESERVE | MEM_COMMIT) : MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  if (!ptr && !addr) [[unlikely]]
    ERROR_LOG("VirtualAlloc(RWX, {}) for internal buffer failed: {}", size, GetLastError());

  return ptr;
}

void MemMap::ReleaseJITMemory(void* ptr, size_t size)
{
  if (!VirtualFree(ptr, 0, MEM_RELEASE))
    ERROR_LOG("Failed to free code pointer {}", static_cast<void*>(ptr));
}

#if defined(CPU_ARCH_ARM32) || defined(CPU_ARCH_ARM64) || defined(CPU_ARCH_RISCV64)

void MemMap::FlushInstructionCache(void* address, size_t size)
{
  ::FlushInstructionCache(GetCurrentProcess(), address, size);
}

#endif

SharedMemoryMappingArea::SharedMemoryMappingArea() = default;

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
  Destroy();
}

SharedMemoryMappingArea::PlaceholderMap::iterator SharedMemoryMappingArea::FindPlaceholder(size_t offset)
{
  if (m_placeholder_ranges.empty())
    return m_placeholder_ranges.end();

  // this will give us an iterator equal or after page
  auto it = m_placeholder_ranges.lower_bound(offset);
  if (it == m_placeholder_ranges.end())
  {
    // check the last page
    it = (++m_placeholder_ranges.rbegin()).base();
  }

  // it's the one we found?
  if (offset >= it->first && offset < it->second)
    return it;

  // otherwise try the one before
  if (it == m_placeholder_ranges.begin())
    return m_placeholder_ranges.end();

  --it;
  if (offset >= it->first && offset < it->second)
    return it;
  else
    return m_placeholder_ranges.end();
}

bool SharedMemoryMappingArea::Create(size_t size)
{
  Destroy();

  AssertMsg(Common::IsAlignedPow2(size, HOST_PAGE_SIZE), "Size is page aligned");

  m_base_ptr = static_cast<u8*>(VirtualAlloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                              PAGE_NOACCESS, nullptr, 0));
  if (!m_base_ptr)
    return false;

  m_size = size;
  m_num_pages = size / HOST_PAGE_SIZE;
  m_placeholder_ranges.emplace(0, size);
  return true;
}

void SharedMemoryMappingArea::Destroy()
{
  AssertMsg(m_num_mappings == 0, "No mappings left");

  // hopefully this will be okay, and we don't need to coalesce all the placeholders...
  if (m_base_ptr && !VirtualFreeEx(GetCurrentProcess(), m_base_ptr, 0, MEM_RELEASE))
    Panic("Failed to release shared memory area");

  m_placeholder_ranges.clear();
  m_base_ptr = nullptr;
  m_size = 0;
  m_num_pages = 0;
  m_num_mappings = 0;
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size,
                                 PageProtect mode)
{
  DebugAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

  const size_t map_offset = static_cast<u8*>(map_base) - m_base_ptr;
  DebugAssert(Common::IsAlignedPow2(map_offset, HOST_PAGE_SIZE));
  DebugAssert(Common::IsAlignedPow2(map_size, HOST_PAGE_SIZE));

  // should be a placeholder. unless there's some other mapping we didn't free.
  PlaceholderMap::iterator phit = FindPlaceholder(map_offset);
  DebugAssertMsg(phit != m_placeholder_ranges.end(), "Page we're mapping is a placeholder");
  DebugAssertMsg(map_offset >= phit->first && map_offset < phit->second, "Page is in returned placeholder range");
  DebugAssertMsg((map_offset + map_size) <= phit->second, "Page range is in returned placeholder range");

  // do we need to split to the left? (i.e. is there a placeholder before this range)
  const size_t old_ph_end = phit->second;
  if (map_offset != phit->first)
  {
    phit->second = map_offset;

    // split it (i.e. left..start and start..end are now separated)
    if (!VirtualFreeEx(GetCurrentProcess(), OffsetPointer(phit->first), (map_offset - phit->first),
                       MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    {
      Panic("Failed to left split placeholder for map");
    }
  }
  else
  {
    // start of the placeholder is getting used, we'll split it right below if there's anything left over
    m_placeholder_ranges.erase(phit);
  }

  // do we need to split to the right? (i.e. is there a placeholder after this range)
  if ((map_offset + map_size) != old_ph_end)
  {
    // split out end..ph_end
    m_placeholder_ranges.emplace(map_offset + map_size, old_ph_end);

    if (!VirtualFreeEx(GetCurrentProcess(), OffsetPointer(map_offset), map_size,
                       MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    {
      Panic("Failed to right split placeholder for map");
    }
  }

  // actually do the mapping, replacing the placeholder on the range
  if (!MapViewOfFile3(static_cast<HANDLE>(file_handle), GetCurrentProcess(), map_base, file_offset, map_size,
                      MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
  {
    ERROR_LOG("MapViewOfFile3() failed: {}", GetLastError());
    return nullptr;
  }

  if (mode != PageProtect::ReadWrite)
  {
    DWORD old_prot;
    if (!VirtualProtect(map_base, map_size, static_cast<DWORD>(mode), &old_prot))
      Panic("Failed to protect memory mapping");
  }

  m_num_mappings++;
  return static_cast<u8*>(map_base);
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size)
{
  DebugAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

  const size_t map_offset = static_cast<u8*>(map_base) - m_base_ptr;
  DebugAssert(Common::IsAlignedPow2(map_offset, HOST_PAGE_SIZE));
  DebugAssert(Common::IsAlignedPow2(map_size, HOST_PAGE_SIZE));

  // unmap the specified range
  if (!UnmapViewOfFile2(GetCurrentProcess(), map_base, MEM_PRESERVE_PLACEHOLDER))
  {
    ERROR_LOG("UnmapViewOfFile2() failed: {}", GetLastError());
    return false;
  }

  // can we coalesce to the left?
  PlaceholderMap::iterator left_it = (map_offset > 0) ? FindPlaceholder(map_offset - 1) : m_placeholder_ranges.end();
  if (left_it != m_placeholder_ranges.end())
  {
    // the left placeholder should end at our start
    DebugAssert(map_offset == left_it->second);
    left_it->second = map_offset + map_size;

    // combine placeholders before and the range we're unmapping, i.e. to the left
    if (!VirtualFreeEx(GetCurrentProcess(), OffsetPointer(left_it->first), left_it->second - left_it->first,
                       MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
    {
      Panic("Failed to coalesce placeholders left for unmap");
    }
  }
  else
  {
    // this is a new placeholder
    left_it = m_placeholder_ranges.emplace(map_offset, map_offset + map_size).first;
  }

  // can we coalesce to the right?
  PlaceholderMap::iterator right_it =
    ((map_offset + map_size) < m_size) ? FindPlaceholder(map_offset + map_size) : m_placeholder_ranges.end();
  if (right_it != m_placeholder_ranges.end())
  {
    // should start at our end
    DebugAssert(right_it->first == (map_offset + map_size));
    left_it->second = right_it->second;
    m_placeholder_ranges.erase(right_it);

    // combine our placeholder and the next, i.e. to the right
    if (!VirtualFreeEx(GetCurrentProcess(), OffsetPointer(left_it->first), left_it->second - left_it->first,
                       MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
    {
      Panic("Failed to coalescae placeholders right for unmap");
    }
  }

  m_num_mappings--;
  return true;
}

#elif defined(__APPLE__)

bool MemMap::MemProtect(void* baseaddr, size_t size, PageProtect mode)
{
  DebugAssertMsg((size & (HOST_PAGE_SIZE - 1)) == 0, "Size is page aligned");

  kern_return_t res = mach_vm_protect(mach_task_self(), reinterpret_cast<mach_vm_address_t>(baseaddr), size, false,
                                      static_cast<vm_prot_t>(mode));
  if (res != KERN_SUCCESS) [[unlikely]]
  {
    ERROR_LOG("mach_vm_protect() failed: {}", res);
    return false;
  }

  return true;
}

std::string MemMap::GetFileMappingName(const char* prefix)
{
  // name actually is not used.
  return {};
}

void* MemMap::CreateSharedMemory(const char* name, size_t size, Error* error)
{
  mach_vm_size_t vm_size = size;
  mach_port_t port;
  const kern_return_t res = mach_make_memory_entry_64(
    mach_task_self(), &vm_size, 0, MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &port, MACH_PORT_NULL);
  if (res != KERN_SUCCESS)
  {
    Error::SetStringFmt(error, "mach_make_memory_entry_64() failed: {}", res);
    return nullptr;
  }

  return reinterpret_cast<void*>(static_cast<uintptr_t>(port));
}

void MemMap::DestroySharedMemory(void* ptr)
{
  mach_port_deallocate(mach_task_self(), static_cast<mach_port_t>(reinterpret_cast<uintptr_t>(ptr)));
}

void MemMap::DeleteSharedMemory(const char* name)
{
}

void* MemMap::MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, PageProtect mode)
{
  mach_vm_address_t ptr = reinterpret_cast<mach_vm_address_t>(baseaddr);
  const kern_return_t res = mach_vm_map(mach_task_self(), &ptr, size, 0, baseaddr ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE,
                                        static_cast<mach_port_t>(reinterpret_cast<uintptr_t>(handle)), offset, FALSE,
                                        static_cast<vm_prot_t>(mode), VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
  if (res != KERN_SUCCESS)
  {
    ERROR_LOG("mach_vm_map() failed: {}", res);
    return nullptr;
  }

  return reinterpret_cast<void*>(ptr);
}

void MemMap::UnmapSharedMemory(void* baseaddr, size_t size)
{
  const kern_return_t res = mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(baseaddr), size);
  if (res != KERN_SUCCESS)
    Panic("Failed to unmap shared memory");
}

const void* MemMap::GetBaseAddress()
{
  u32 name_buffer_size = 0;
  _NSGetExecutablePath(nullptr, &name_buffer_size);
  if (name_buffer_size > 0) [[likely]]
  {
    std::unique_ptr<char[]> name_buffer = std::make_unique_for_overwrite<char[]>(name_buffer_size + 1);
    if (_NSGetExecutablePath(name_buffer.get(), &name_buffer_size) == 0) [[likely]]
    {
      name_buffer[name_buffer_size] = 0;

      const struct segment_command_64* command = getsegbyname("__TEXT");
      if (command) [[likely]]
      {
        const u8* base = reinterpret_cast<const u8*>(command->vmaddr);
        const u32 image_count = _dyld_image_count();
        for (u32 i = 0; i < image_count; i++)
        {
          if (std::strcmp(_dyld_get_image_name(i), name_buffer.get()) == 0)
            return base + _dyld_get_image_vmaddr_slide(i);
        }
      }
    }
  }

  return reinterpret_cast<const void*>(&GetBaseAddress);
}

void* MemMap::AllocateJITMemoryAt(const void* addr, size_t size)
{
#if !defined(__aarch64__)
  kern_return_t ret = mach_vm_allocate(mach_task_self(), reinterpret_cast<mach_vm_address_t*>(&addr), size,
                                       addr ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE);
  if (ret != KERN_SUCCESS)
  {
    ERROR_LOG("mach_vm_allocate() returned {}", ret);
    return nullptr;
  }

  ret = mach_vm_protect(mach_task_self(), reinterpret_cast<mach_vm_address_t>(addr), size, false,
                        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
  if (ret != KERN_SUCCESS)
  {
    ERROR_LOG("mach_vm_protect() returned {}", ret);
    mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(addr), size);
    return nullptr;
  }

  return const_cast<void*>(addr);
#else
  // On ARM64, we need to use MAP_JIT, which means we can't use MAP_FIXED.
  if (addr)
    return nullptr;

  constexpr int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT;
  void* ptr = mmap(const_cast<void*>(addr), size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
  if (ptr == MAP_FAILED)
  {
    ERROR_LOG("mmap(RWX, {}) for internal buffer failed: {}", size, errno);
    return nullptr;
  }

  return ptr;
#endif
}

void MemMap::ReleaseJITMemory(void* ptr, size_t size)
{
#if !defined(__aarch64__)
  const kern_return_t res = mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(ptr), size);
  if (res != KERN_SUCCESS)
    ERROR_LOG("mach_vm_deallocate() failed: {}", res);
#else
  if (munmap(ptr, size) != 0)
    ERROR_LOG("Failed to free code pointer {}", static_cast<void*>(ptr));
#endif
}

#if defined(CPU_ARCH_ARM32) || defined(CPU_ARCH_ARM64) || defined(CPU_ARCH_RISCV64)

void MemMap::FlushInstructionCache(void* address, size_t size)
{
  __builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address) + size);
}

#endif

SharedMemoryMappingArea::SharedMemoryMappingArea() = default;

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
  Destroy();
}

bool SharedMemoryMappingArea::Create(size_t size)
{
  AssertMsg(Common::IsAlignedPow2(size, HOST_PAGE_SIZE), "Size is page aligned");
  Destroy();

  const kern_return_t res =
    mach_vm_map(mach_task_self(), reinterpret_cast<mach_vm_address_t*>(&m_base_ptr), size, 0, VM_FLAGS_ANYWHERE,
                MEMORY_OBJECT_NULL, 0, false, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE);
  if (res != KERN_SUCCESS)
  {
    ERROR_LOG("mach_vm_map() failed: {}", res);
    return false;
  }

  m_size = size;
  m_num_pages = size / HOST_PAGE_SIZE;
  return true;
}

void SharedMemoryMappingArea::Destroy()
{
  AssertMsg(m_num_mappings == 0, "No mappings left");

  if (m_base_ptr &&
      mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(m_base_ptr), m_size) != KERN_SUCCESS)
  {
    Panic("Failed to release shared memory area");
  }

  m_base_ptr = nullptr;
  m_size = 0;
  m_num_pages = 0;
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size,
                                 PageProtect mode)
{
  DebugAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

  const kern_return_t res =
    mach_vm_map(mach_task_self(), reinterpret_cast<mach_vm_address_t*>(&map_base), map_size, 0, VM_FLAGS_OVERWRITE,
                static_cast<mach_port_t>(reinterpret_cast<uintptr_t>(file_handle)), file_offset, false,
                static_cast<vm_prot_t>(mode), VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
  if (res != KERN_SUCCESS) [[unlikely]]
  {
    ERROR_LOG("mach_vm_map() failed: {}", res);
    return nullptr;
  }

  m_num_mappings++;
  return static_cast<u8*>(map_base);
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size)
{
  DebugAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

  const kern_return_t res =
    mach_vm_map(mach_task_self(), reinterpret_cast<mach_vm_address_t*>(&map_base), map_size, 0, VM_FLAGS_OVERWRITE,
                MEMORY_OBJECT_NULL, 0, false, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE);
  if (res != KERN_SUCCESS) [[unlikely]]
  {
    ERROR_LOG("mach_vm_map() failed: {}", res);
    return false;
  }

  m_num_mappings--;
  return true;
}

#ifdef __aarch64__

static thread_local int s_code_write_depth = 0;

void MemMap::BeginCodeWrite()
{
  // DEBUG_LOG("BeginCodeWrite(): {}", s_code_write_depth);
  if ((s_code_write_depth++) == 0)
  {
    // DEBUG_LOG("  pthread_jit_write_protect_np(0)");
    pthread_jit_write_protect_np(0);
  }
}

void MemMap::EndCodeWrite()
{
  // DEBUG_LOG("EndCodeWrite(): {}", s_code_write_depth);

  DebugAssert(s_code_write_depth > 0);
  if ((--s_code_write_depth) == 0)
  {
    // DEBUG_LOG("  pthread_jit_write_protect_np(1)");
    pthread_jit_write_protect_np(1);
  }
}

#endif

#elif !defined(__ANDROID__)

bool MemMap::MemProtect(void* baseaddr, size_t size, PageProtect mode)
{
  DebugAssertMsg((size & (HOST_PAGE_SIZE - 1)) == 0, "Size is page aligned");

  const int result = mprotect(baseaddr, size, static_cast<int>(mode));
  if (result != 0) [[unlikely]]
  {
    ERROR_LOG("mprotect() for {} at {} failed", size, baseaddr);
    return false;
  }

  return true;
}

std::string MemMap::GetFileMappingName(const char* prefix)
{
  const unsigned pid = static_cast<unsigned>(getpid());
#if defined(__FreeBSD__)
  // FreeBSD's shm_open(3) requires name to be absolute
  return fmt::format("/tmp/{}_{}", prefix, pid);
#else
  return fmt::format("{}_{}", prefix, pid);
#endif
}

void* MemMap::CreateSharedMemory(const char* name, size_t size, Error* error)
{
  const bool is_anonymous = (!name || *name == 0);
#if defined(__linux__) || defined(__FreeBSD__)
  const int fd = is_anonymous ? memfd_create("", 0) : shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
  {
    Error::SetErrno(error, is_anonymous ? "memfd_create() failed: " : "shm_open() failed: ", errno);
    return nullptr;
  }
#else
  const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
  {
    Error::SetErrno(error, "shm_open() failed: ", errno);
    return nullptr;
  }

  // we're not going to be opening this mapping in other processes, so remove the file
  if (is_anonymous)
    shm_unlink(name);
#endif

  // use fallocate() to ensure we don't SIGBUS later on.
#ifdef __linux__
  if (fallocate(fd, 0, 0, static_cast<off_t>(size)) < 0)
  {
    Error::SetErrno(error, TinyString::from_format("fallocate({}) failed: ", size), errno);
    close(fd);
    if (!is_anonymous)
      shm_unlink(name);
    return nullptr;
  }
#else
  // ensure it's the correct size
  if (ftruncate(fd, static_cast<off_t>(size)) < 0)
  {
    Error::SetErrno(error, TinyString::from_format("ftruncate({}) failed: ", size), errno);
    close(fd);
    if (!is_anonymous)
      shm_unlink(name);
    return nullptr;
  }
#endif

  return reinterpret_cast<void*>(static_cast<intptr_t>(fd));
}

void MemMap::DestroySharedMemory(void* ptr)
{
  close(static_cast<int>(reinterpret_cast<intptr_t>(ptr)));
}

void MemMap::DeleteSharedMemory(const char* name)
{
  shm_unlink(name);
}

void* MemMap::MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, PageProtect mode)
{
  const int flags = (baseaddr != nullptr) ? (MAP_SHARED | MAP_FIXED) : MAP_SHARED;
  void* ptr = mmap(baseaddr, size, static_cast<int>(mode), flags, static_cast<int>(reinterpret_cast<intptr_t>(handle)),
                   static_cast<off_t>(offset));
  if (ptr == MAP_FAILED)
    return nullptr;

  return ptr;
}

void MemMap::UnmapSharedMemory(void* baseaddr, size_t size)
{
  if (munmap(baseaddr, size) != 0)
    Panic("Failed to unmap shared memory");
}

const void* MemMap::GetBaseAddress()
{
#ifndef __APPLE__
  Dl_info info;
  if (dladdr(reinterpret_cast<const void*>(&GetBaseAddress), &info) == 0)
  {
    ERROR_LOG("dladdr() failed");
    return nullptr;
  }

  return info.dli_fbase;
#else
#error Fixme
#endif
}

void* MemMap::AllocateJITMemoryAt(const void* addr, size_t size)
{
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__linux__)
  // Linux does the right thing, allows us to not disturb an existing mapping.
  if (addr)
    flags |= MAP_FIXED_NOREPLACE;
#elif defined(__FreeBSD__)
  // FreeBSD achieves the same with MAP_FIXED and MAP_EXCL.
  if (addr)
    flags |= MAP_FIXED | MAP_EXCL;
#else
  // Targeted mapping not available?
  if (addr)
    return nullptr;
#endif

  void* ptr = mmap(const_cast<void*>(addr), size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
  if (ptr == MAP_FAILED)
  {
    if (!addr)
      ERROR_LOG("mmap(RWX, {}) for internal buffer failed: {}", size, errno);

    return nullptr;
  }
  else if (addr && ptr != addr) [[unlikely]]
  {
    if (munmap(ptr, size) != 0)
      ERROR_LOG("Failed to munmap() incorrectly hinted allocation: {}", errno);
    return nullptr;
  }

  return ptr;
}

void MemMap::ReleaseJITMemory(void* ptr, size_t size)
{
  if (munmap(ptr, size) != 0)
    ERROR_LOG("Failed to free code pointer {}", static_cast<void*>(ptr));
}

#if defined(CPU_ARCH_ARM32) || defined(CPU_ARCH_ARM64) || defined(CPU_ARCH_RISCV64)

void MemMap::FlushInstructionCache(void* address, size_t size)
{
  __builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address) + size);
}

#endif

SharedMemoryMappingArea::SharedMemoryMappingArea() = default;

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
  Destroy();
}

bool SharedMemoryMappingArea::Create(size_t size)
{
  AssertMsg(Common::IsAlignedPow2(size, HOST_PAGE_SIZE), "Size is page aligned");
  Destroy();

  void* alloc = mmap(nullptr, size, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (alloc == MAP_FAILED)
    return false;

  m_base_ptr = static_cast<u8*>(alloc);
  m_size = size;
  m_num_pages = size / HOST_PAGE_SIZE;
  return true;
}

void SharedMemoryMappingArea::Destroy()
{
  AssertMsg(m_num_mappings == 0, "No mappings left");

  if (m_base_ptr && munmap(m_base_ptr, m_size) != 0)
    Panic("Failed to release shared memory area");

  m_base_ptr = nullptr;
  m_size = 0;
  m_num_pages = 0;
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size,
                                 PageProtect mode)
{
  DebugAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

  void* const ptr = mmap(map_base, map_size, static_cast<int>(mode), MAP_SHARED | MAP_FIXED,
                         static_cast<int>(reinterpret_cast<intptr_t>(file_handle)), static_cast<off_t>(file_offset));
  if (ptr == MAP_FAILED)
    return nullptr;

  m_num_mappings++;
  return static_cast<u8*>(ptr);
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size)
{
  DebugAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

  if (mmap(map_base, map_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
    return false;

  m_num_mappings--;
  return true;
}

#endif

void* MemMap::AllocateJITMemory(size_t size)
{
  const u8* base =
    reinterpret_cast<const u8*>(Common::AlignDownPow2(reinterpret_cast<uintptr_t>(GetBaseAddress()), HOST_PAGE_SIZE));
  u8* ptr = nullptr;
#if !defined(CPU_ARCH_ARM64) || !defined(__APPLE__)

#if defined(CPU_ARCH_X64)
  static constexpr size_t assume_binary_size = 64 * 1024 * 1024;
  static constexpr size_t step = 64 * 1024 * 1024;
  static constexpr size_t max_displacement = 0x80000000u;
#elif defined(CPU_ARCH_ARM64) || defined(CPU_ARCH_RISCV64)
  static constexpr size_t assume_binary_size = 16 * 1024 * 1024;
  static constexpr size_t step = 8 * 1024 * 1024;
  static constexpr size_t max_displacement =
    1024 * 1024 * 1024; // technically 4GB, but we don't want to spend that much time trying
#elif defined(CPU_ARCH_ARM32)
  static constexpr size_t assume_binary_size = 8 * 1024 * 1024; // Wishful thinking...
  static constexpr size_t step = 2 * 1024 * 1024;
  static constexpr size_t max_displacement = 32 * 1024 * 1024;
#else
#error Unhandled architecture.
#endif

  const size_t max_displacement_from_start = max_displacement - size;
  Assert(size <= max_displacement);

  // Try to find a region in the max displacement range of the process base address.
  // Assume that the DuckStation binary will at max be some size, release is currently around 12MB on Windows.
  // Therefore the max offset is +/- 12MB + code_size. Try allocating in steps by incrementing the pointer, then if no
  // address range is found, go backwards from the base address (which will probably fail).
  const u8* min_address =
    base - std::min(reinterpret_cast<ptrdiff_t>(base), static_cast<ptrdiff_t>(max_displacement_from_start));
  const u8* max_address = base + max_displacement_from_start;
  VERBOSE_LOG("Base address: {}", static_cast<const void*>(base));
  VERBOSE_LOG("Acceptable address range: {} - {}", static_cast<const void*>(min_address),
              static_cast<const void*>(max_address));

  // Start offset by the expected binary size.
  for (const u8* current_address = base + assume_binary_size;; current_address += step)
  {
    VERBOSE_LOG("Trying {} (displacement 0x{:X})", static_cast<const void*>(current_address),
                static_cast<ptrdiff_t>(current_address - base));
    if ((ptr = static_cast<u8*>(AllocateJITMemoryAt(current_address, size))))
      break;

    if ((reinterpret_cast<uintptr_t>(current_address) + step) > reinterpret_cast<uintptr_t>(max_address) ||
        (reinterpret_cast<uintptr_t>(current_address) + step) < reinterpret_cast<uintptr_t>(current_address))
    {
      break;
    }
  }

  // Try before (will likely fail).
  if (!ptr && reinterpret_cast<uintptr_t>(base) >= step)
  {
    for (const u8* current_address = base - step;; current_address -= step)
    {
      VERBOSE_LOG("Trying {} (displacement 0x{:X})", static_cast<const void*>(current_address),
                  static_cast<ptrdiff_t>(base - current_address));
      if ((ptr = static_cast<u8*>(AllocateJITMemoryAt(current_address, size))))
        break;

      if ((reinterpret_cast<uintptr_t>(current_address) - step) < reinterpret_cast<uintptr_t>(min_address) ||
          (reinterpret_cast<uintptr_t>(current_address) - step) > reinterpret_cast<uintptr_t>(current_address))
      {
        break;
      }
    }
  }

  if (!ptr)
  {
#ifdef CPU_ARCH_X64
    ERROR_LOG("Failed to allocate JIT buffer in range, expect crashes.");
#endif
    if (!(ptr = static_cast<u8*>(AllocateJITMemoryAt(nullptr, size))))
      return ptr;
  }
#else
  // We cannot control where the buffer gets allocated on Apple Silicon. Hope for the best.
  if (!(ptr = static_cast<u8*>(AllocateJITMemoryAt(nullptr, size))))
    return ptr;
#endif

  INFO_LOG("Allocated JIT buffer of size {} at {} (0x{:X} bytes / {} MB away)", size, static_cast<void*>(ptr),
           std::abs(static_cast<ptrdiff_t>(ptr - base)),
           (std::abs(static_cast<ptrdiff_t>(ptr - base)) + (1024 * 1024 - 1)) / (1024 * 1024));

  return ptr;
}
