#pragma once
#include "common/types.h"
#include <atomic>
#include <optional>

namespace Common {
class MemoryArena
{
public:
  class View
  {
  public:
    enum : size_t
    {
      RESERVED_REGION_OFFSET = static_cast<size_t>(-1)
    };

    View(MemoryArena* parent, void* base_pointer, size_t arena_offset, size_t mapping_size, bool writable);
    View(View&& view);
    ~View();

    void* GetBasePointer() const { return m_base_pointer; }
    size_t GetArenaOffset() const { return m_arena_offset; }
    size_t GetMappingSize() const { return m_mapping_size; }
    bool IsWritable() const { return m_writable; }

  private:
    MemoryArena* m_parent;
    void* m_base_pointer;
    size_t m_arena_offset;
    size_t m_mapping_size;
    bool m_writable;
  };

  MemoryArena();
  ~MemoryArena();

  static void* FindBaseAddressForMapping(size_t size);

  ALWAYS_INLINE size_t GetSize() const { return m_size; }
  ALWAYS_INLINE bool IsWritable() const { return m_writable; }
  ALWAYS_INLINE bool IsExecutable() const { return m_executable; }

  bool IsValid() const;
  bool Create(size_t size, bool writable, bool executable);
  void Destroy();

  std::optional<View> CreateView(size_t offset, size_t size, bool writable, bool executable,
                                 void* fixed_address = nullptr);

  std::optional<View> CreateReservedView(size_t size,  void* fixed_address = nullptr);

  void* CreateViewPtr(size_t offset, size_t size, bool writable, bool executable, void* fixed_address = nullptr);
  bool FlushViewPtr(void* address, size_t size);
  bool ReleaseViewPtr(void* address, size_t size);

  void* CreateReservedPtr(size_t size, void* fixed_address = nullptr);
  bool ReleaseReservedPtr(void* address, size_t size);

  static bool SetPageProtection(void* address, size_t length, bool readable, bool writable, bool executable);

private:
#if defined(_WIN32)
  void* m_file_handle = nullptr;
#elif defined(__linux__) || defined(ANDROID) || defined(__APPLE__) || defined(__FreeBSD__)
  int m_shmem_fd = -1;
#endif

  std::atomic_size_t m_num_views{0};
  size_t m_size = 0;
  bool m_writable = false;
  bool m_executable = false;
};
} // namespace Common
