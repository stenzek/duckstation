#pragma once

#ifdef _MSC_VER

__declspec(align(16)) struct fastjmp_buf
{
  unsigned __int64 Rip;
  unsigned __int64 Rbx;
  unsigned __int64 Rsp;
  unsigned __int64 Rbp;
  unsigned __int64 Rsi;
  unsigned __int64 Rdi;
  unsigned __int64 R12;
  unsigned __int64 R13;
  unsigned __int64 R14;
  unsigned __int64 R15;
  unsigned __int64 Xmm6[2];
  unsigned __int64 Xmm7[2];
  unsigned __int64 Xmm8[2];
  unsigned __int64 Xmm9[2];
  unsigned __int64 Xmm10[2];
  unsigned __int64 Xmm11[2];
  unsigned __int64 Xmm12[2];
  unsigned __int64 Xmm13[2];
  unsigned __int64 Xmm14[2];
  unsigned __int64 Xmm15[2];
  //   unsigned long MxCsr;
  //   unsigned short FpCsr;
  //   unsigned short Spare;
};

extern "C" {
void fastjmp_set(fastjmp_buf*);
void fastjmp_jmp(fastjmp_buf*);
}

#else

#include <setjmp.h>
#define fastjmp_buf jmp_buf
#define fastjmp_set(buf) setjmp(*(buf))
#define fastjmp_jmp(buf) longjmp(*(buf), 0)

#endif
