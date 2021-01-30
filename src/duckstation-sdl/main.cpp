#ifdef _WIN32
#include "common/windows_headers.h"
#endif

#include <cstdio>
#include <cstdlib>

static const char message[] = "The DuckStation SDL frontend has been removed and replaced with the Fullscreen UI.\n\n"
                              "Please use duckstation-qt instead.";

#ifdef _WIN32

int WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
  MessageBoxA(nullptr, message, "DuckStation", MB_OK | MB_ICONERROR);
  return -1;
}

#else

int main(int argc, char* argv[])
{
  std::fputs(message, stderr);
  return -1;
}

#endif
