#include "core/host_interface.h"
#include <jni.h>

#define DEFINE_JNI_METHOD(return_type, name, ...)                                                                      \
  extern "C" JNIEXPORT return_type JNICALL Java_com_github_stenzek_duckstation_##name(__VA_ARGS__)

DEFINE_JNI_METHOD(bool, createSystem)
{
  return false;
}

DEFINE_JNI_METHOD(bool, bootSystem, const char* filename, const char* state_filename)
{
  return false;
}

DEFINE_JNI_METHOD(void, runFrame) {}