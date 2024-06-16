#include "scriptengine.h"
#include "cpu_core.h"
#include "fullscreen_ui.h"
#include "host.h"
#include "system.h"

#include "common/error.h"
#include "common/log.h"
#include "common/small_string.h"

#include "fmt/format.h"

#ifdef _DEBUG
#undef _DEBUG
#include "Python.h"
#define _DEBUG
#else
#include "Python.h"
#endif

#include <mutex>

Log_SetChannel(ScriptEngine);

// TODO: File loading
// TODO: Expose imgui
// TODO: hexdump, assembler, disassembler
// TODO: save/load state
// TODO: controller inputs
// TODO: Intepreter reset

namespace ScriptEngine {
static void SetErrorFromStatus(Error* error, const PyStatus& status, std::string_view prefix);
static void SetPyErrFromError(Error* error);
static bool RedirectOutput(Error* error);
static void WriteOutput(std::string_view message);
static bool CheckVMValid();

static PyObject* output_redirector_write(PyObject* self, PyObject* args);

static PyObject* dspy_inittab();

static PyObject* dspy_exit(PyObject* self, PyObject* args);

static PyObject* dspy_vm_valid(PyObject* self, PyObject* args);
static PyObject* dspy_vm_start(PyObject* self, PyObject* args, PyObject* kwargs);
static PyObject* dspy_vm_pause(PyObject* self, PyObject* args);
static PyObject* dspy_vm_resume(PyObject* self, PyObject* args);
static PyObject* dspy_vm_reset(PyObject* self, PyObject* args);
static PyObject* dspy_vm_shutdown(PyObject* self, PyObject* args);

static PyMethodDef s_vm_methods[] = {
  {"valid", dspy_vm_valid, METH_NOARGS, "Returns true if a virtual machine is active."},
  {"start", reinterpret_cast<PyCFunction>(dspy_vm_start), METH_VARARGS | METH_KEYWORDS,
   "Starts a new virtual machine with the specified arguments."},
  {"pause", dspy_vm_pause, METH_NOARGS, "Pauses VM if it is not currently running."},
  {"resume", dspy_vm_resume, METH_NOARGS, "Resumes VM if it is currently paused."},
  {"reset", dspy_vm_reset, METH_NOARGS, "Resets current VM."},
  {"shutdown", dspy_vm_shutdown, METH_NOARGS, "Shuts down current VM."},
  {},
};

template<typename T>
static PyObject* dspy_mem_readT(PyObject* self, PyObject* args);
template<typename T>
static PyObject* dspy_mem_writeT(PyObject* self, PyObject* args);

static PyMethodDef s_mem_methods[] = {
  {"read8", dspy_mem_readT<u8>, METH_VARARGS, "Reads a byte from the specified address."},
  {"reads8", dspy_mem_readT<u8>, METH_VARARGS, "Reads a signed byte from the specified address."},
  {"read16", dspy_mem_readT<u16>, METH_VARARGS, "Reads a halfword from the specified address."},
  {"reads16", dspy_mem_readT<s16>, METH_VARARGS, "Reads a signed halfword from the specified address."},
  {"read32", dspy_mem_readT<u32>, METH_VARARGS, "Reads a word from the specified address."},
  {"reads32", dspy_mem_readT<s32>, METH_VARARGS, "Reads a word from the specified address."},
  {"write8", dspy_mem_writeT<u8>, METH_VARARGS, "Reads a byte from the specified address."},
  {"write16", dspy_mem_writeT<u16>, METH_VARARGS, "Reads a halfword from the specified address."},
  {"write32", dspy_mem_writeT<u32>, METH_VARARGS, "Reads a word from the specified address."},
  {},
};

template<typename... T>
ALWAYS_INLINE static void WriteOutput(fmt::format_string<T...> fmt, T&&... args)
{
  SmallString message;
  fmt::vformat_to(std::back_inserter(message), fmt, fmt::make_format_args(args...));
  WriteOutput(message);
}

static std::mutex s_output_mutex;
static OutputCallback s_output_callback;
static void* s_output_callback_userdata;

static const char* INITIALIZATION_SCRIPT = "import dspy;"
                                           "from dspy import vm;"
                                           "from dspy import mem;";

} // namespace ScriptEngine

void ScriptEngine::SetErrorFromStatus(Error* error, const PyStatus& status, std::string_view prefix)
{
  Error::SetStringFmt(error, "func={} err_msg={} exitcode={}", prefix, status.func ? status.func : "",
                      status.err_msg ? status.err_msg : "", status.exitcode);
}

void ScriptEngine::SetPyErrFromError(Error* error)
{
  PyErr_SetString(PyExc_RuntimeError, error ? error->GetDescription().c_str() : "unknown error");
}

bool ScriptEngine::Initialize(Error* error)
{
  PyPreConfig pre_config;
  PyPreConfig_InitIsolatedConfig(&pre_config);
  pre_config.utf8_mode = true;

  PyStatus status = Py_PreInitialize(&pre_config);
  if (PyStatus_IsError(status)) [[unlikely]]
  {
    SetErrorFromStatus(error, status, "Py_PreInitialize() failed: ");
    Shutdown();
    return false;
  }

  if (const int istatus = PyImport_AppendInittab("dspy", &dspy_inittab); istatus != 0)
  {
    Error::SetStringFmt(error, "PyImport_AppendInittab() failed: {}", istatus);
    Shutdown();
    return false;
  }

  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.pythonpath_env = Py_DecodeLocale("C:\\Users\\Me\\AppData\\Local\\Programs\\Python\\Python311\\Lib", nullptr);

  status = Py_InitializeFromConfig(&config);

  PyMem_RawFree(config.pythonpath_env);

  if (PyStatus_IsError(status)) [[unlikely]]
  {
    SetErrorFromStatus(error, status, "Py_InitializeFromConfig() failed: ");
    Shutdown();
    return false;
  }

  if (!RedirectOutput(error)) [[unlikely]]
  {
    Error::AddPrefix(error, "Failed to redirect output: ");
    Shutdown();
    return false;
  }

  if (PyRun_SimpleString(INITIALIZATION_SCRIPT) < 0)
  {
    PyErr_Print();
    Error::SetStringFmt(error, "Failed to run initialization script.");
    Shutdown();
    return false;
  }

  return true;
}

void ScriptEngine::Shutdown()
{
  if (const int ret = Py_FinalizeEx(); ret != 0)
  {
    ERROR_LOG("Py_FinalizeEx() returned {}", ret);
  }
}

void ScriptEngine::SetOutputCallback(OutputCallback callback, void* userdata)
{
  std::unique_lock lock(s_output_mutex);
  s_output_callback = callback;
  s_output_callback_userdata = userdata;
}

bool ScriptEngine::RedirectOutput(Error* error)
{
  PyObject* dspy_module = PyImport_ImportModule("dspy");
  if (!dspy_module)
  {
    Error::SetStringView(error, "PyImport_ImportModule(dspy) failed");
    return false;
  }

  PyObject* module_dict = PyModule_GetDict(dspy_module);
  if (!module_dict)
  {
    Error::SetStringView(error, "PyModule_GetDict() failed");
    Py_DECREF(dspy_module);
    return false;
  }

  PyObject* output_redirector_class = PyDict_GetItemString(module_dict, "output_redirector");
  Py_DECREF(dspy_module);
  if (!output_redirector_class)
  {
    Error::SetStringView(error, "PyDict_GetItemString() failed");
    return false;
  }

  PyObject* output_redirector;
  if (!PyCallable_Check(output_redirector_class) ||
      !(output_redirector = PyObject_CallObject(output_redirector_class, nullptr)))
  {
    Error::SetStringView(error, "PyObject_CallObject() failed");
    Py_DECREF(output_redirector_class);
    return false;
  }

  Py_DECREF(output_redirector_class);

  PyObject* sys_module = PyImport_ImportModule("sys");
  if (!sys_module)
  {
    Error::SetStringView(error, "PyImport_ImportModule(sys) failed");
    Py_DECREF(output_redirector);
    return false;
  }

  module_dict = PyModule_GetDict(sys_module);
  if (!module_dict)
  {
    Error::SetStringView(error, "PyModule_GetDict(sys) failed");
    Py_DECREF(sys_module);
    Py_DECREF(output_redirector);
    return false;
  }

  if (PyDict_SetItemString(module_dict, "stdout", output_redirector) < 0 ||
      PyDict_SetItemString(module_dict, "stderr", output_redirector) < 0)
  {
    Error::SetStringView(error, "PyDict_SetItemString() failed");
    Py_DECREF(sys_module);
    Py_DECREF(output_redirector);
    return false;
  }

  Py_DECREF(sys_module);
  Py_DECREF(output_redirector);
  return true;
}

void ScriptEngine::WriteOutput(std::string_view message)
{
  INFO_LOG("Python: {}", message);

  if (s_output_callback)
  {
    std::unique_lock lock(s_output_mutex);
    s_output_callback(message, s_output_callback_userdata);
  }
}

void ScriptEngine::EvalString(const char* str)
{
  WriteOutput(">>> {}\n", str);

  const int res = PyRun_SimpleString(str);
  if (res == 0)
    return;

  WriteOutput("PyRun_SimpleString() returned {}\n", res);
  PyErr_Print();
}

#define PYBOOL(b) ((b) ? Py_NewRef(Py_True) : Py_NewRef(Py_False))

PyObject* ScriptEngine::output_redirector_write(PyObject* self, PyObject* args)
{
  const char* msg;
  if (!PyArg_ParseTuple(args, "s", &msg))
    return nullptr;

  WriteOutput(msg);
  Py_RETURN_NONE;
}

PyObject* ScriptEngine::dspy_inittab()
{
  static PyMethodDef root_methods[] = {
    {"exit", dspy_exit, METH_NOARGS, "Exits the hosting application."},
    {},
  };
  static PyModuleDef root_module_def = {
    PyModuleDef_HEAD_INIT, "dspy", nullptr, -1, root_methods, nullptr, nullptr, nullptr, nullptr};

  static PyModuleDef vm_module_def = {
    PyModuleDef_HEAD_INIT, "vm", nullptr, -1, s_vm_methods, nullptr, nullptr, nullptr, nullptr};

  static PyModuleDef mem_module_def = {
    PyModuleDef_HEAD_INIT, "mem", nullptr, -1, s_mem_methods, nullptr, nullptr, nullptr, nullptr};

  static PyMethodDef output_redirectory_methods[] = {
    {"write", output_redirector_write, METH_VARARGS, "Writes to script console."},
    {},
  };

  static PyTypeObject output_redirector = {
    .ob_base = PyVarObject_HEAD_INIT(nullptr, 0).tp_name = "dspy.output_redirector",
    .tp_basicsize = sizeof(PyObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = PyDoc_STR("Output Redirector"),
    .tp_methods = output_redirectory_methods,
    .tp_new = PyType_GenericNew,
  };

  PyObject* root_module = PyModule_Create(&root_module_def);
  if (!root_module)
    return nullptr;

  PyObject* vm_module = PyModule_Create(&vm_module_def);
  if (!vm_module)
  {
    Py_DECREF(root_module);
    return nullptr;
  }

  PyObject* mem_module = PyModule_Create(&mem_module_def);
  if (!vm_module)
  {
    Py_DECREF(vm_module);
    Py_DECREF(root_module);
    return nullptr;
  }

  if (PyType_Ready(&output_redirector) < 0 || PyModule_AddObjectRef(root_module, "vm", vm_module) < 0 ||
      PyModule_AddObjectRef(root_module, "mem", mem_module) < 0 ||
      PyModule_AddObjectRef(root_module, "output_redirector", reinterpret_cast<PyObject*>(&output_redirector)) < 0)
  {
    Py_DECREF(mem_module);
    Py_DECREF(vm_module);
    Py_DECREF(root_module);
    return nullptr;
  }

  Py_DECREF(mem_module);
  Py_DECREF(vm_module);
  return root_module;
}

PyObject* ScriptEngine::dspy_exit(PyObject* self, PyObject* args)
{
  Host::RequestExitApplication(false);
  Py_RETURN_NONE;
}

bool ScriptEngine::CheckVMValid()
{
  if (System::IsValid())
  {
    return true;
  }
  else
  {
    PyErr_SetString(PyExc_RuntimeError, "VM has not been started.");
    return false;
  }
}

PyObject* ScriptEngine::dspy_vm_valid(PyObject* self, PyObject* args)
{
  return PYBOOL(System::IsValid());
}

PyObject* ScriptEngine::dspy_vm_start(PyObject* self, PyObject* args, PyObject* kwargs)
{
  static constexpr const char* kwlist[] = {
    "path", "savestate", "exe", "override_fastboot", "override_slowboot", "start_fullscreen", "start_paused", nullptr};

  if (System::GetState() != System::State::Shutdown)
  {
    PyErr_SetString(PyExc_RuntimeError, "VM has already been started.");
    return nullptr;
  }

  const char* path = nullptr;
  const char* savestate = nullptr;
  const char* override_exe = nullptr;
  int override_fastboot = 0;
  int override_slowboot = 0;
  int start_fullscreen = 0;
  int start_paused = 0;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|$ssspppp", const_cast<char**>(kwlist), &path, &savestate,
                                   &override_exe, &override_fastboot, &override_slowboot, &start_fullscreen,
                                   &start_paused))
  {
    return nullptr;
  }

  SystemBootParameters params;
  if (path)
    params.filename = path;
  if (savestate)
    params.save_state = savestate;
  if (override_exe)
    params.override_exe = override_exe;
  if (override_fastboot)
    params.override_fast_boot = true;
  else if (override_slowboot)
    params.override_fast_boot = false;
  if (start_fullscreen)
    params.override_fullscreen = true;
  if (start_paused)
    params.override_start_paused = true;

  WriteOutput("Starting system with path={}\n", params.filename);

  Error error;
  if (!System::BootSystem(std::move(params), &error))
  {
    WriteOutput("Starting system failed: {}\n", error.GetDescription());
    SetPyErrFromError(&error);
    return nullptr;
  }

  Py_RETURN_NONE;
}

PyObject* ScriptEngine::dspy_vm_pause(PyObject* self, PyObject* args)
{
  if (!CheckVMValid())
    return nullptr;

  System::PauseSystem(true);
  Py_RETURN_NONE;
}

PyObject* ScriptEngine::dspy_vm_resume(PyObject* self, PyObject* args)
{
  if (!CheckVMValid())
    return nullptr;

  System::PauseSystem(false);
  Py_RETURN_NONE;
}

PyObject* ScriptEngine::dspy_vm_reset(PyObject* self, PyObject* args)
{
  if (!CheckVMValid())
    return nullptr;

  System::ResetSystem();
  Py_RETURN_NONE;
}

PyObject* ScriptEngine::dspy_vm_shutdown(PyObject* self, PyObject* args)
{
  if (!CheckVMValid())
    return nullptr;

  System::ShutdownSystem(false);
  Py_RETURN_NONE;
}

template<typename T>
PyObject* ScriptEngine::dspy_mem_readT(PyObject* self, PyObject* args)
{
  if (!CheckVMValid())
    return nullptr;

  unsigned int address;
  if (!PyArg_ParseTuple(args, "I", &address))
    return nullptr;

  if constexpr (std::is_same_v<T, u8> || std::is_same_v<T, s8>)
  {
    u8 result;
    if (CPU::SafeReadMemoryByte(address, &result)) [[likely]]
      return std::is_signed_v<T> ? PyLong_FromLong(static_cast<s8>(result)) : PyLong_FromUnsignedLong(result);
  }
  else if constexpr (std::is_same_v<T, u16> || std::is_same_v<T, s16>)
  {
    u16 result;
    if (CPU::SafeReadMemoryHalfWord(address, &result)) [[likely]]
      return std::is_signed_v<T> ? PyLong_FromLong(static_cast<s16>(result)) : PyLong_FromUnsignedLong(result);
  }
  else if constexpr (std::is_same_v<T, u32> || std::is_same_v<T, s32>)
  {
    u32 result;
    if (CPU::SafeReadMemoryWord(address, &result)) [[likely]]
      return std::is_signed_v<T> ? PyLong_FromLong(static_cast<s32>(result)) : PyLong_FromUnsignedLong(result);
  }

  PyErr_SetString(PyExc_RuntimeError, "Address was not valid.");
  return nullptr;
}

template PyObject* ScriptEngine::dspy_mem_readT<u8>(PyObject*, PyObject*);
template PyObject* ScriptEngine::dspy_mem_readT<s8>(PyObject*, PyObject*);
template PyObject* ScriptEngine::dspy_mem_readT<u16>(PyObject*, PyObject*);
template PyObject* ScriptEngine::dspy_mem_readT<s16>(PyObject*, PyObject*);
template PyObject* ScriptEngine::dspy_mem_readT<u32>(PyObject*, PyObject*);
template PyObject* ScriptEngine::dspy_mem_readT<s32>(PyObject*, PyObject*);

template<typename T>
PyObject* ScriptEngine::dspy_mem_writeT(PyObject* self, PyObject* args)
{
  if (!CheckVMValid())
    return nullptr;

  unsigned int address;
  long long value;
  if (!PyArg_ParseTuple(args, "IL", &address, &value))
    return nullptr;

  if constexpr (std::is_same_v<T, u8>)
  {
    if (CPU::SafeWriteMemoryByte(address, static_cast<u8>(value))) [[likely]]
      Py_RETURN_NONE;
  }
  else if constexpr (std::is_same_v<T, u16>)
  {
    if (CPU::SafeWriteMemoryHalfWord(address, static_cast<u16>(value))) [[likely]]
      Py_RETURN_NONE;
  }
  else
  {
    if (CPU::SafeWriteMemoryWord(address, static_cast<u32>(value))) [[likely]]
      Py_RETURN_NONE;
  }

  PyErr_SetString(PyExc_RuntimeError, "Address was not valid.");
  return nullptr;
}

template PyObject* ScriptEngine::dspy_mem_writeT<u8>(PyObject*, PyObject*);
template PyObject* ScriptEngine::dspy_mem_writeT<u16>(PyObject*, PyObject*);
template PyObject* ScriptEngine::dspy_mem_writeT<u32>(PyObject*, PyObject*);
