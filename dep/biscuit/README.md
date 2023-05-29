# Biscuit: RISC-V Runtime Code Generation Library

*RISC it for the biscuit*

## About

An experimental runtime code generator for RISC-V.

This allows for runtime code generation of RISC-V instructions. Similar
to how [Xbyak](https://github.com/herumi/xbyak) allows for runtime code generation of x86 instructions.


## Implemented ISA Features

Includes both 32-bit and 64-bit instructions in the following:

| Feature   | Version |
|:----------|:-------:|
| A         | 2.1     |
| B         | 1.0     |
| C         | 2.0     |
| D         | 2.2     |
| F         | 2.2     |
| H         | 1.0 RC  |
| K         | 1.0.1   |
| M         | 2.0     |
| N         | 1.1     |
| Q         | 2.2     |
| RV32I     | 2.1     |
| RV64I     | 2.1     |
| S         | 1.12    |
| V         | 1.0     |
| Sstc      | 0.5.4   |
| Zfh       | 1.0     |
| Zfhmin    | 1.0     |
| Zicbom    | 1.0     |
| Zicbop    | 1.0     |
| Zicboz    | 1.0     |
| Zicsr     | 2.0     |
| Zifencei  | 2.0     |
| Zihintntl | 0.2     |

Note that usually only extensions considered ratified will be implemented
as non-ratified documents are considerably more likely to have
large changes made to them, which makes maintaining instruction
APIs a little annoying.


## Dependencies

Biscuit requires no external dependencies for its library other than the C++ standard library. 
The tests, however, use the Catch2 testing library. This is included in tree so there's no need
to worry about installing it yourself if you wish to run said tests.


## Building Biscuit

1. Generate the build files for the project with CMake
2. Hit the build button in your IDE of choice, or run the relevant console command to build for the CMake generator you've chosen.
3. Done.


## Running Tests

1. Generate the build files for the project with CMake
2. Build the tests
3. Run the test executable directly, or enter `ctest` into your terminal.


## License

The library is licensed under the MIT license.

While it's not a requirement whatsoever, it'd be pretty neat if you told me that you found the library useful :-)


## Example

The following is an adapted equivalent of the `strlen` implementation within the RISC-V bit manipulation extension specification.
For brevity, it has been condensed to only handle little-endian platforms.

```cpp
// We prepare some contiguous buffer and give the pointer to the beginning
// of the data and the total size of the buffer in bytes to the assembler.

void strlen_example(uint8_t* buffer, size_t buffer_size) {
    using namespace biscuit;

    constexpr int ptrlog = 3;
    constexpr int szreg  = 8;

    Assembler as(buffer, buffer_size);
    Label done;
    Label loop;

    as.ANDI(a3, a0, szreg - 1); // Offset
    as.ANDI(a1, a0, 0xFF8);     // Align pointer

    as.LI(a4, szreg);
    as.SUB(a4, a4, a3);         // XLEN - offset
    as.SLLI(a3, a3, ptrlog);    // offset * 8
    as.LD(a2, 0, a1);           // Chunk

    //
    // Shift the partial/unaligned chunk we loaded to remove the bytes
    // from before the start of the string, adding NUL bytes at the end.
    //
    as.SRL(a2, a2, a3);         // chunk >> (offset * 8)
    as.ORCB(a2, a2);
    as.NOT(a2, a2);

    // Non-NUL bytes in the string have been expanded to 0x00, while
    // NUL bytes have become 0xff. Search for the first set bit
    // (corresponding to a NUL byte in the original chunk).
    as.CTZ(a2, a2);

    // The first chunk is special: compare against the number of valid
    // bytes in this chunk.
    as.SRLI(a0, a2, 3);
    as.BGTU(a4, a0, &done);
    as.ADDI(a3, a1, szreg);
    as.LI(a4, -1);

    // Our critical loop is 4 instructions and processes data in 4 byte
    // or 8 byte chunks.
    as.Bind(&loop);

    as.LD(a2, szreg, a1);
    as.ADDI(a1, a1, szreg);
    as.ORCB(a2, a2);
    as.BEQ(a2, a4, &loop);

    as.NOT(a2, a2);
    as.CTZ(a2, a2);
    as.SUB(a1, a1, a3);
    as.ADD(a0, a0, a1);
    as.SRLI(a2, a2, 3);
    as.ADD(a0, a0, a2);

    as.Bind(&done);

    as.RET();
}
```
