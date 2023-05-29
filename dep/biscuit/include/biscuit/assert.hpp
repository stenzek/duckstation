#pragma once

#include <cstdio>
#include <cstdlib>

#define BISCUIT_ASSERT(condition)                                        \
  do {                                                                   \
    if (!(condition)) {                                                  \
      std::printf("Assertion failed (%s)\nin %s, function %s line %i\n", \
                  #condition,                                            \
                  __FILE__, __func__, __LINE__);                         \
      std::abort();                                                      \
    }                                                                    \
  } while (false)
