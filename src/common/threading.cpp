#include "threading.h"
#include "assert.h"
#include <memory>

#if !defined(_WIN32) && !defined(__APPLE__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#if defined(_WIN32)
#include "windows_headers.h"
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#include <sys/prctl.h>
#include <sys/types.h>

// glibc < v2.30 doesn't define gettid...
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#endif
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#include <pthread_np.h>
#endif
#endif

#ifdef _WIN32
union FileTimeU64Union
{
  FILETIME filetime;
  u64 u64time;
};
#endif

#ifdef __APPLE__
// gets the CPU time used by the current thread (both system and user), in
// microseconds, returns 0 on failure
static u64 getthreadtime(thread_port_t thread)
{
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  thread_basic_info_data_t info;

  kern_return_t kr = thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
  if (kr != KERN_SUCCESS)
    return 0;

  // add system and user time
  return (u64)info.user_time.seconds * (u64)1e6 + (u64)info.user_time.microseconds +
         (u64)info.system_time.seconds * (u64)1e6 + (u64)info.system_time.microseconds;
}
#endif

#ifdef __linux__
// Helper function to get either either the current cpu usage
// in called thread or in id thread
static u64 get_thread_time(void* id = 0)
{
  clockid_t cid;
  if (id)
  {
    int err = pthread_getcpuclockid((pthread_t)id, &cid);
    if (err)
      return 0;
  }
  else
  {
    cid = CLOCK_THREAD_CPUTIME_ID;
  }

  struct timespec ts;
  int err = clock_gettime(cid, &ts);
  if (err)
    return 0;

  return (u64)ts.tv_sec * (u64)1e6 + (u64)ts.tv_nsec / (u64)1e3;
}
#endif

void Threading::Timeslice()
{
#if defined(_WIN32)
  ::Sleep(0);
#elif defined(__APPLE__)
  sched_yield();
#else
  sched_yield();
#endif
}

Threading::ThreadHandle::ThreadHandle() = default;

#ifdef _WIN32
Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
{
  if (handle.m_native_handle)
  {
    HANDLE new_handle;
    if (DuplicateHandle(GetCurrentProcess(), (HANDLE)handle.m_native_handle, GetCurrentProcess(), &new_handle,
                        THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, 0))
    {
      m_native_handle = (void*)new_handle;
    }
  }
}
#else
Threading::ThreadHandle::ThreadHandle(const ThreadHandle& handle)
  : m_native_handle(handle.m_native_handle)
#ifdef __linux__
    ,
    m_native_id(handle.m_native_id)
#endif
{
}
#endif

#ifdef _WIN32
Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle) : m_native_handle(handle.m_native_handle)
{
  handle.m_native_handle = nullptr;
}
#else
Threading::ThreadHandle::ThreadHandle(ThreadHandle&& handle)
  : m_native_handle(handle.m_native_handle)
#ifdef __linux__
    ,
    m_native_id(handle.m_native_id)
#endif
{
  handle.m_native_handle = nullptr;
#ifdef __linux__
  handle.m_native_id = 0;
#endif
}
#endif

Threading::ThreadHandle::~ThreadHandle()
{
#ifdef _WIN32
  if (m_native_handle)
    CloseHandle(m_native_handle);
#endif
}

Threading::ThreadHandle Threading::ThreadHandle::GetForCallingThread()
{
  ThreadHandle ret;
#ifdef _WIN32
  ret.m_native_handle =
    (void*)OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
#else
  ret.m_native_handle = (void*)pthread_self();
#ifdef __linux__
  ret.m_native_id = gettid();
#endif
#endif
  return ret;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(ThreadHandle&& handle)
{
#ifdef _WIN32
  if (m_native_handle)
    CloseHandle((HANDLE)m_native_handle);
  m_native_handle = handle.m_native_handle;
  handle.m_native_handle = nullptr;
#else
  m_native_handle = handle.m_native_handle;
  handle.m_native_handle = nullptr;
#ifdef __linux__
  m_native_id = handle.m_native_id;
  handle.m_native_id = 0;
#endif
#endif
  return *this;
}

Threading::ThreadHandle& Threading::ThreadHandle::operator=(const ThreadHandle& handle)
{
#ifdef _WIN32
  if (m_native_handle)
  {
    CloseHandle((HANDLE)m_native_handle);
    m_native_handle = nullptr;
  }

  HANDLE new_handle;
  if (DuplicateHandle(GetCurrentProcess(), (HANDLE)handle.m_native_handle, GetCurrentProcess(), &new_handle,
                      THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, 0))
  {
    m_native_handle = (void*)new_handle;
  }
#else
  m_native_handle = handle.m_native_handle;
#ifdef __linux__
  m_native_id = handle.m_native_id;
#endif
#endif

  return *this;
}

u64 Threading::ThreadHandle::GetCPUTime() const
{
#if defined(_WIN32) && !defined(_UWP) && !defined(_M_ARM64)
  u64 ret = 0;
  if (m_native_handle)
    QueryThreadCycleTime((HANDLE)m_native_handle, &ret);
  return ret;
#elif defined(_WIN32)
  FileTimeU64Union user = {}, kernel = {};
  FILETIME dummy;
  GetThreadTimes((HANDLE)m_native_handle, &dummy, &dummy, &kernel.filetime, &user.filetime);
  return user.u64time + kernel.u64time;
#elif defined(__APPLE__)
  return getthreadtime(pthread_mach_thread_np((pthread_t)m_native_handle));
#elif defined(__linux__)
  return get_thread_time(m_native_handle);
#else
  return 0;
#endif
}

bool Threading::ThreadHandle::SetAffinity(u64 processor_mask) const
{
#if defined(_WIN32)
  if (processor_mask == 0)
    processor_mask = ~processor_mask;

  return (SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)processor_mask) != 0 || GetLastError() != ERROR_SUCCESS);
#elif defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);

  if (processor_mask != 0)
  {
    for (u32 i = 0; i < 64; i++)
    {
      if (processor_mask & (static_cast<u64>(1) << i))
      {
        CPU_SET(i, &set);
      }
    }
  }
  else
  {
    long num_processors = sysconf(_SC_NPROCESSORS_CONF);
    for (long i = 0; i < num_processors; i++)
    {
      CPU_SET(i, &set);
    }
  }

  return sched_setaffinity((pid_t)m_native_id, sizeof(set), &set) >= 0;
#else
  return false;
#endif
}

Threading::Thread::Thread() = default;

Threading::Thread::Thread(Thread&& thread) : ThreadHandle(thread), m_stack_size(thread.m_stack_size)
{
  thread.m_stack_size = 0;
}

Threading::Thread::Thread(EntryPoint func) : ThreadHandle()
{
  if (!Start(std::move(func)))
    Panic("Failed to start implicitly started thread.");
}

Threading::Thread::~Thread()
{
  AssertMsg(!m_native_handle, "Thread should be detached or joined at destruction");
}

void Threading::Thread::SetStackSize(u32 size)
{
  AssertMsg(!m_native_handle, "Can't change the stack size on a started thread");
  m_stack_size = size;
}

#if defined(_WIN32)

unsigned Threading::Thread::ThreadProc(void* param)
{
  std::unique_ptr<EntryPoint> entry(static_cast<EntryPoint*>(param));
  (*entry.get())();
  return 0;
}

bool Threading::Thread::Start(EntryPoint func)
{
  AssertMsg(!m_native_handle, "Can't start an already-started thread");

  std::unique_ptr<EntryPoint> func_clone(std::make_unique<EntryPoint>(std::move(func)));
  unsigned thread_id;
  m_native_handle =
    reinterpret_cast<void*>(_beginthreadex(nullptr, m_stack_size, ThreadProc, func_clone.get(), 0, &thread_id));
  if (!m_native_handle)
    return false;

  // thread started, it'll release the memory
  func_clone.release();
  return true;
}

#elif defined(__linux__)

// For Linux, we have to do a bit of trickery here to get the thread's ID back from
// the thread itself, because it's not part of pthreads. We use a semaphore to signal
// when the thread has started, and filled in thread_id_ptr.
struct ThreadProcParameters
{
  Threading::Thread::EntryPoint func;
  Threading::KernelSemaphore* start_semaphore;
  unsigned int* thread_id_ptr;
};

void* Threading::Thread::ThreadProc(void* param)
{
  std::unique_ptr<ThreadProcParameters> entry(static_cast<ThreadProcParameters*>(param));
  *entry->thread_id_ptr = gettid();
  entry->start_semaphore->Post();
  entry->func();
  return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
  AssertMsg(!m_native_handle, "Can't start an already-started thread");

  KernelSemaphore start_semaphore;
  std::unique_ptr<ThreadProcParameters> params(std::make_unique<ThreadProcParameters>());
  params->func = std::move(func);
  params->start_semaphore = &start_semaphore;
  params->thread_id_ptr = &m_native_id;

  pthread_attr_t attrs;
  bool has_attributes = false;

  if (m_stack_size != 0)
  {
    has_attributes = true;
    pthread_attr_init(&attrs);
  }
  if (m_stack_size != 0)
    pthread_attr_setstacksize(&attrs, m_stack_size);

  pthread_t handle;
  const int res = pthread_create(&handle, has_attributes ? &attrs : nullptr, ThreadProc, params.get());
  if (res != 0)
    return false;

  // wait until it sets our native id
  start_semaphore.Wait();

  // thread started, it'll release the memory
  m_native_handle = (void*)handle;
  params.release();
  return true;
}

#else

void* Threading::Thread::ThreadProc(void* param)
{
  std::unique_ptr<EntryPoint> entry(static_cast<EntryPoint*>(param));
  (*entry.get())();
  return nullptr;
}

bool Threading::Thread::Start(EntryPoint func)
{
  pxAssertRel(!m_native_handle, "Can't start an already-started thread");

  std::unique_ptr<EntryPoint> func_clone(std::make_unique<EntryPoint>(std::move(func)));

  pthread_attr_t attrs;
  bool has_attributes = false;

  if (m_stack_size != 0)
  {
    has_attributes = true;
    pthread_attr_init(&attrs);
  }
  if (m_stack_size != 0)
    pthread_attr_setstacksize(&attrs, m_stack_size);

  pthread_t handle;
  const int res = pthread_create(&handle, has_attributes ? &attrs : nullptr, ThreadProc, func_clone.get());
  if (res != 0)
    return false;

  // thread started, it'll release the memory
  m_native_handle = (void*)handle;
  func_clone.release();
  return true;
}

#endif

void Threading::Thread::Detach()
{
  AssertMsg(m_native_handle, "Can't detach without a thread");
#ifdef _WIN32
  CloseHandle((HANDLE)m_native_handle);
  m_native_handle = nullptr;
#else
  pthread_detach((pthread_t)m_native_handle);
  m_native_handle = nullptr;
#ifdef __linux__
  m_native_id = 0;
#endif
#endif
}

void Threading::Thread::Join()
{
  AssertMsg(m_native_handle, "Can't join without a thread");
#ifdef _WIN32
  const DWORD res = WaitForSingleObject((HANDLE)m_native_handle, INFINITE);
  if (res != WAIT_OBJECT_0)
    Panic("WaitForSingleObject() for thread join failed");

  CloseHandle((HANDLE)m_native_handle);
  m_native_handle = nullptr;
#else
  void* retval;
  const int res = pthread_join((pthread_t)m_native_handle, &retval);
  if (res != 0)
    Panic("pthread_join() for thread join failed");

  m_native_handle = nullptr;
#ifdef __linux__
  m_native_id = 0;
#endif
#endif
}

Threading::ThreadHandle& Threading::Thread::operator=(Thread&& thread)
{
  ThreadHandle::operator=(thread);
  m_stack_size = thread.m_stack_size;
  thread.m_stack_size = 0;
  return *this;
}

u64 Threading::GetThreadCpuTime()
{
#if defined(_WIN32) && !defined(_UWP) && !defined(_M_ARM64)
  u64 ret = 0;
  QueryThreadCycleTime(GetCurrentThread(), &ret);
  return ret;
#elif defined(_WIN32)
  FileTimeU64Union user = {}, kernel = {};
  FILETIME dummy;
  GetThreadTimes(GetCurrentThread(), &dummy, &dummy, &kernel.filetime, &user.filetime);
  return user.u64time + kernel.u64time;
#elif defined(__APPLE__)
  return getthreadtime(pthread_mach_thread_np(pthread_self()));
#else
  return get_thread_time(nullptr);
#endif
}

u64 Threading::GetThreadTicksPerSecond()
{
#if defined(_WIN32) && !defined(_UWP) && !defined(_M_ARM64)
  // On x86, despite what the MS documentation says, this basically appears to be rdtsc.
  // So, the frequency is our base clock speed (and stable regardless of power management).
  static u64 frequency = 0;
  if (UNLIKELY(frequency == 0))
  {
    frequency = 1000000;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) ==
        ERROR_SUCCESS)
    {
      DWORD value;
      DWORD value_size = sizeof(value);
      if (RegQueryValueExW(hKey, L"~MHz", 0, nullptr, reinterpret_cast<LPBYTE>(&value), &value_size) == ERROR_SUCCESS)
      {
        // value is in mhz, convert to hz
        frequency *= value;
      }

      RegCloseKey(hKey);
    }
  }

  return frequency;
#elif defined(_WIN32)
  return 10000000;
#elif defined(__APPLE__)
  return 1000000;

#else
  return 1000000;
#endif
}

void Threading::SetNameOfCurrentThread(const char* name)
{
  // This feature needs Windows headers and MSVC's SEH support:

#if defined(_WIN32) && defined(_MSC_VER)

  // This code sample was borrowed form some obscure MSDN article.
  // In a rare bout of sanity, it's an actual Microsoft-published hack
  // that actually works!

  static const int MS_VC_EXCEPTION = 0x406D1388;

#pragma pack(push, 8)
  struct THREADNAME_INFO
  {
    DWORD dwType;     // Must be 0x1000.
    LPCSTR szName;    // Pointer to name (in user addr space).
    DWORD dwThreadID; // Thread ID (-1=caller thread).
    DWORD dwFlags;    // Reserved for future use, must be zero.
  };
#pragma pack(pop)

  THREADNAME_INFO info;
  info.dwType = 0x1000;
  info.szName = name;
  info.dwThreadID = GetCurrentThreadId();
  info.dwFlags = 0;

  __try
  {
    RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
  }
#elif defined(__linux__)
  // Extract of manpage: "The name can be up to 16 bytes long, and should be
  //						null-terminated if it contains fewer bytes."
  prctl(PR_SET_NAME, name, 0, 0, 0);
#else
  pthread_set_name_np(pthread_self(), name);
#endif
}

Threading::KernelSemaphore::KernelSemaphore()
{
#ifdef _WIN32
  m_sema = CreateSemaphore(nullptr, 0, LONG_MAX, nullptr);
#elif defined(__APPLE__)
  semaphore_create(mach_task_self(), &m_sema, SYNC_POLICY_FIFO, 0);
#else
  sem_init(&m_sema, false, 0);
#endif
}

Threading::KernelSemaphore::~KernelSemaphore()
{
#ifdef _WIN32
  CloseHandle(m_sema);
#elif defined(__APPLE__)
  semaphore_destroy(mach_task_self(), m_sema);
#else
  sem_destroy(&m_sema);
#endif
}

void Threading::KernelSemaphore::Post()
{
#ifdef _WIN32
  ReleaseSemaphore(m_sema, 1, nullptr);
#elif defined(__APPLE__)
  semaphore_signal(m_sema);
#else
  sem_post(&m_sema);
#endif
}

void Threading::KernelSemaphore::Wait()
{
#ifdef _WIN32
  WaitForSingleObject(m_sema, INFINITE);
#elif defined(__APPLE__)
  semaphore_wait(m_sema);
#else
  sem_wait(&m_sema);
#endif
}

bool Threading::KernelSemaphore::TryWait()
{
#ifdef _WIN32
  return WaitForSingleObject(m_sema, 0) == WAIT_OBJECT_0;
#elif defined(__APPLE__)
  mach_timespec_t time = {};
  kern_return_t res = semaphore_timedwait(m_sema, time);
  return (res != KERN_OPERATION_TIMED_OUT);
#else
  return sem_trywait(&m_sema) == 0;
#endif
}
