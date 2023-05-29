// Copyright (c), 2022, KNS Group LLC (YADRO)
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#include <biscuit/cpuinfo.hpp>

namespace biscuit {

bool CPUInfo::Has(RISCVExtension extension) const {
#if defined(__linux__) && defined(__riscv)
    const static uint64_t features = getauxval(AT_HWCAP) & (
                            COMPAT_HWCAP_ISA_I |
                            COMPAT_HWCAP_ISA_M |
                            COMPAT_HWCAP_ISA_A |
                            COMPAT_HWCAP_ISA_F |
                            COMPAT_HWCAP_ISA_D |
                            COMPAT_HWCAP_ISA_C |
                            COMPAT_HWCAP_ISA_V
    );
#else
    const static uint64_t features = 0;
#endif

    return (features & static_cast<uint64_t>(extension)) != 0;
}

uint32_t CPUInfo::GetVlenb() const {
    if(Has(RISCVExtension::V)) {
        static CSRReader<CSR::VLenb> csrReader;
        const static auto getVLEN = csrReader.GetCode<uint32_t (*)()>();
        return getVLEN();
    }

    return 0;
}

} // namespace biscuit
