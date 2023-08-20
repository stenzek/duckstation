
# Xbyak 6.73 [![Badge Build]][Build Status]

*A C++ JIT assembler for x86 (IA32), x64 (AMD64, x86-64)*

## Menu

- [Install]
- [Usage]
- [Changelog]

## Abstract

Xbyak is a C++ header library that enables dynamically to assemble x86(IA32), x64(AMD64, x86-64) mnemonic.

The pronunciation of Xbyak is `kəi-bja-k`.
It is named from a Japanese word [開闢](https://translate.google.com/?hl=ja&sl=ja&tl=en&text=%E9%96%8B%E9%97%A2&op=translate), which means the beginning of the world.

## Feature

- header file only
- Intel/MASM like syntax
- fully support AVX-512

**Note**:
Use `and_()`, `or_()`, ... instead of `and()`, `or()`.
If you want to use them, then specify `-fno-operator-names` option to gcc/clang.

### Derived Projects
- [Xbyak_aarch64](https://github.com/fujitsu/xbyak_aarch64/) : for AArch64
- [Xbyak_riscv](https://github.com/herumi/xbyak_riscv) : for RISC-V

### News

- add amx_fp16/avx_vnni_int8/avx_ne_convert/avx-ifma
- add movdiri, movdir64b, clwb, cldemote
- WAITPKG instructions (tpause, umonitor, umwait) are supported.
- MmapAllocator supports memfd with user-defined strings. see sample/memfd.cpp
- strictly check address offset disp32 in a signed 32-bit integer. e.g., `ptr[(void*)0xffffffff]` causes an error.
  - define `XBYAK_OLD_DISP_CHECK` if you need an old check, but the option will be remoevd.
- add `jmp(mem, T_FAR)`, `call(mem, T_FAR)` `retf()` for far absolute indirect jump.
- vnni instructions such as vpdpbusd supports vex encoding.
- (break backward compatibility) `push(byte, imm)` (resp. `push(word, imm)`) forces to cast `imm` to 8(resp. 16) bit.
- (Windows) `#include <winsock2.h>` has been removed from xbyak.h, so add it explicitly if you need it.
- support exception-less mode see. [Exception-less mode](#exception-less-mode)
- `XBYAK_USE_MMAP_ALLOCATOR` will be defined on Linux/macOS unless `XBYAK_DONT_USE_MMAP_ALLOCATOR` is defined.

### Supported OS

- Windows (Xp, Vista, 7, 10, 11) (32 / 64 bit)
- Linux (32 / 64 bit)
- macOS (Intel CPU)

### Supported Compilers

Almost C++03 or later compilers for x86/x64 such as Visual Studio, g++, clang++, Intel C++ compiler and g++ on mingw/cygwin.

## License

[BSD-3-Clause License](http://opensource.org/licenses/BSD-3-Clause)

## Author

#### 光成滋生 Mitsunari Shigeo
 [GitHub](https://github.com/herumi) | [Website (Japanese)](http://herumi.in.coocan.jp/) | [herumi@nifty.com](mailto:herumi@nifty.com)

## Sponsors welcome
[GitHub Sponsor](https://github.com/sponsors/herumi)

<!----------------------------------------------------------------------------->

[Badge Build]: https://github.com/herumi/xbyak/actions/workflows/main.yml/badge.svg
[Build Status]: https://github.com/herumi/xbyak/actions/workflows/main.yml

[License]: COPYRIGHT

[Changelog]: doc/changelog.md
[Install]: doc/install.md
[Usage]: doc/usage.md

