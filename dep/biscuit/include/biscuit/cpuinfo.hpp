// Copyright (c), 2022, KNS Group LLC (YADRO)
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#pragma once

#include <biscuit/assembler.hpp>
#include <biscuit/registers.hpp>
#include <cstddef>
#include <cstdint>

#if defined(__linux__) && defined(__riscv)
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <asm/hwcap.h>
#endif

namespace biscuit {

#ifndef COMPAT_HWCAP_ISA_I
#define COMPAT_HWCAP_ISA_I  (1U << ('I' - 'A'))
#endif

#ifndef COMPAT_HWCAP_ISA_M
#define COMPAT_HWCAP_ISA_M  (1U << ('M' - 'A'))
#endif

#ifndef COMPAT_HWCAP_ISA_A
#define COMPAT_HWCAP_ISA_A  (1U << ('A' - 'A'))
#endif

#ifndef COMPAT_HWCAP_ISA_F
#define COMPAT_HWCAP_ISA_F  (1U << ('F' - 'A'))
#endif

#ifndef COMPAT_HWCAP_ISA_D
#define COMPAT_HWCAP_ISA_D  (1U << ('D' - 'A'))
#endif

#ifndef COMPAT_HWCAP_ISA_C
#define COMPAT_HWCAP_ISA_C  (1U << ('C' - 'A'))
#endif

#ifndef COMPAT_HWCAP_ISA_V
#define COMPAT_HWCAP_ISA_V  (1U << ('V' - 'A'))
#endif

enum class RISCVExtension : uint64_t {
    I = COMPAT_HWCAP_ISA_I,
    M = COMPAT_HWCAP_ISA_M,
    A = COMPAT_HWCAP_ISA_A,
    F = COMPAT_HWCAP_ISA_F,
    D = COMPAT_HWCAP_ISA_D,
    C = COMPAT_HWCAP_ISA_C,
    V = COMPAT_HWCAP_ISA_V
};

template <CSR csr>
struct CSRReader : public biscuit::Assembler {
    // Buffer capacity exactly for 2 instructions.
    static constexpr size_t capacity = 8;

    CSRReader() : biscuit::Assembler{CSRReader::capacity} {
        CSRR(a0, csr);
        RET();
    }

    // Copy constructor and assignment.
    CSRReader(const CSRReader&) = delete;
    CSRReader& operator=(const CSRReader&) = delete;

    // Move constructor and assignment.
    CSRReader(CSRReader&&) = default;
    CSRReader& operator=(CSRReader&&) = default;

    template <typename CSRReaderFunc>
    CSRReaderFunc GetCode() {
        this->GetCodeBuffer().SetExecutable();
        return reinterpret_cast<CSRReaderFunc>(this->GetBufferPointer(0));
    }
};

/**
 * Class that detects information about a RISC-V CPU.
 */
class CPUInfo {
public:
    /**
     * Checks if a particular RISC-V extension is available.
     *
     * @param extension The extension to check.
     */
    bool Has(RISCVExtension extension) const;

    /// Returns the vector register length in bytes.
    uint32_t GetVlenb() const;
};

} // namespace biscuit
