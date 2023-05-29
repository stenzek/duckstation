#pragma once

#include <cstdint>

namespace biscuit {

// Control and Status Register
enum class CSR : uint32_t {
    // clang-format off

    // User-level CSRs

    UStatus        = 0x000, // User status register
    UIE            = 0x004, // User interrupt-enable register
    UTVEC          = 0x005, // User trap handler base address
    UScratch       = 0x040, // Scratch register for user trap handlers
    UEPC           = 0x041, // User exception program counter
    UCause         = 0x042, // User trap cause
    UTVal          = 0x043, // User bad address or instruction
    UIP            = 0x044, // User interrupt pending

    FFlags         = 0x001, // Floating-point Accrued Exceptions
    FRM            = 0x002, // Floating-point Dynamic Rounding Mode
    FCSR           = 0x003, // Floating-point Control and Status Register (frm + fflags)

    Cycle          = 0xC00, // Cycle counter for RDCYCLE instruction.
    Time           = 0xC01, // Timer for RDTIME instruction.
    InstRet        = 0xC02, // Instructions retired counter for RDINSTRET instruction.
    HPMCounter3    = 0xC03, // Performance-monitoring counter.
    HPMCounter4    = 0xC04, // Performance-monitoring counter.
    HPMCounter5    = 0xC05, // Performance-monitoring counter.
    HPMCounter6    = 0xC06, // Performance-monitoring counter.
    HPMCounter7    = 0xC07, // Performance-monitoring counter.
    HPMCounter8    = 0xC08, // Performance-monitoring counter.
    HPMCounter9    = 0xC09, // Performance-monitoring counter.
    HPMCounter10   = 0xC0A, // Performance-monitoring counter.
    HPMCounter11   = 0xC0B, // Performance-monitoring counter.
    HPMCounter12   = 0xC0C, // Performance-monitoring counter.
    HPMCounter13   = 0xC0D, // Performance-monitoring counter.
    HPMCounter14   = 0xC0E, // Performance-monitoring counter.
    HPMCounter15   = 0xC0F, // Performance-monitoring counter.
    HPMCounter16   = 0xC10, // Performance-monitoring counter.
    HPMCounter17   = 0xC11, // Performance-monitoring counter.
    HPMCounter18   = 0xC12, // Performance-monitoring counter.
    HPMCounter19   = 0xC13, // Performance-monitoring counter.
    HPMCounter20   = 0xC14, // Performance-monitoring counter.
    HPMCounter21   = 0xC15, // Performance-monitoring counter.
    HPMCounter22   = 0xC16, // Performance-monitoring counter.
    HPMCounter23   = 0xC17, // Performance-monitoring counter.
    HPMCounter24   = 0xC18, // Performance-monitoring counter.
    HPMCounter25   = 0xC19, // Performance-monitoring counter.
    HPMCounter26   = 0xC1A, // Performance-monitoring counter.
    HPMCounter27   = 0xC1B, // Performance-monitoring counter.
    HPMCounter28   = 0xC1C, // Performance-monitoring counter.
    HPMCounter29   = 0xC1D, // Performance-monitoring counter.
    HPMCounter30   = 0xC1E, // Performance-monitoring counter.
    HPMCounter31   = 0xC1F, // Performance-monitoring counter.
    CycleH         = 0xC80, // Upper 32 bits of cycle, RV32I only.
    TimeH          = 0xC81, // Upper 32 bits of time, RV32I only.
    InstRetH       = 0xC82, // Upper 32 bits of instret, RV32I only.
    HPMCounter3H   = 0xC83, // Upper 32 bits of HPMCounter3, RV32I only.
    HPMCounter4H   = 0xC84, // Upper 32 bits of HPMCounter4, RV32I only.
    HPMCounter5H   = 0xC85, // Upper 32 bits of HPMCounter5, RV32I only.
    HPMCounter6H   = 0xC86, // Upper 32 bits of HPMCounter6, RV32I only.
    HPMCounter7H   = 0xC87, // Upper 32 bits of HPMCounter7, RV32I only.
    HPMCounter8H   = 0xC88, // Upper 32 bits of HPMCounter8, RV32I only.
    HPMCounter9H   = 0xC89, // Upper 32 bits of HPMCounter9, RV32I only.
    HPMCounter10H  = 0xC8A, // Upper 32 bits of HPMCounter10, RV32I only.
    HPMCounter11H  = 0xC8B, // Upper 32 bits of HPMCounter11, RV32I only.
    HPMCounter12H  = 0xC8C, // Upper 32 bits of HPMCounter12, RV32I only.
    HPMCounter13H  = 0xC8D, // Upper 32 bits of HPMCounter13, RV32I only.
    HPMCounter14H  = 0xC8E, // Upper 32 bits of HPMCounter14, RV32I only.
    HPMCounter15H  = 0xC8F, // Upper 32 bits of HPMCounter15, RV32I only.
    HPMCounter16H  = 0xC90, // Upper 32 bits of HPMCounter16, RV32I only.
    HPMCounter17H  = 0xC91, // Upper 32 bits of HPMCounter17, RV32I only.
    HPMCounter18H  = 0xC92, // Upper 32 bits of HPMCounter18, RV32I only.
    HPMCounter19H  = 0xC93, // Upper 32 bits of HPMCounter19, RV32I only.
    HPMCounter20H  = 0xC94, // Upper 32 bits of HPMCounter20, RV32I only.
    HPMCounter21H  = 0xC95, // Upper 32 bits of HPMCounter21, RV32I only.
    HPMCounter22H  = 0xC96, // Upper 32 bits of HPMCounter22, RV32I only.
    HPMCounter23H  = 0xC97, // Upper 32 bits of HPMCounter23, RV32I only.
    HPMCounter24H  = 0xC98, // Upper 32 bits of HPMCounter24, RV32I only.
    HPMCounter25H  = 0xC99, // Upper 32 bits of HPMCounter25, RV32I only.
    HPMCounter26H  = 0xC9A, // Upper 32 bits of HPMCounter26, RV32I only.
    HPMCounter27H  = 0xC9B, // Upper 32 bits of HPMCounter27, RV32I only.
    HPMCounter28H  = 0xC9C, // Upper 32 bits of HPMCounter28, RV32I only.
    HPMCounter29H  = 0xC9D, // Upper 32 bits of HPMCounter29, RV32I only.
    HPMCounter30H  = 0xC9E, // Upper 32 bits of HPMCounter30, RV32I only.
    HPMCounter31H  = 0xC9F, // Upper 32 bits of HPMCounter31, RV32I only.

    // Supervisor-level CSRs

    SStatus        = 0x100, // Supervisor status register
    SEDeleg        = 0x102, // Supervisor exception delegation register
    SIDeleg        = 0x103, // Supervisor interrupt delegation register
    SIE            = 0x104, // Supervisor interrupt-enable register
    STVec          = 0x105, // Supervisor trap handler base address
    SCounterEn     = 0x106, // Supervisor counter enable

    SEnvCfg        = 0x10A, // Supervisor environment configuration register

    SScratch       = 0x140, // Scratch register for supervisor trap handlers
    SEPC           = 0x141, // Supervisor exception program counter
    SCause         = 0x142, // Supervisor trap cause
    STVal          = 0x143, // Supervisor bad address or instruction
    SIP            = 0x144, // Supervisor interrupt pending.

    STimeCmp       = 0x14D, // Supervisor timer register
    STimeCmpH      = 0x15D, // Supervisor timer register, RV32 only

    SATP           = 0x180, // Supervisor address translation and protection

    SContext       = 0x5A8, // Supervisor-mode context register

    // Hypervisor-level CSRs

    HStatus        = 0x600, // Hypervisor status register
    HEDeleg        = 0x602, // Hypervisor exception delegation register
    HIDeleg        = 0x603, // Hypervisor interrupt delegation register
    HIE            = 0x604, // Hypervisor interrupt-enable register
    HCounterEn     = 0x606, // Hypervisor counter enable
    HGEIE          = 0x607, // Hypervisor guest external interrupt-enable register

    HTVal          = 0x643, // Hypervisor bad guest physical address
    HIP            = 0x644, // Hypervisor interrupt pending
    HVIP           = 0x645, // Hypervisor virtual interrupt pending
    HTInst         = 0x64A, // Hypervisor trap instruction (transformed)
    HGEIP          = 0xE12, // Hypervisor guest external interrupt pending

    HEnvCfg        = 0x60A, // Hypervisor environment configuration register
    HEnvCfgH       = 0x61A, // Additional hypervisor environment configuration register, RV32 only

    HGATP          = 0x680, // Hypervisor guest address translation and protection

    HContext       = 0x6A8, // Hypervisor-mode context register

    HTimeDelta     = 0x605, // Delta for VS/VU-mode timer
    HTimeDeltaH    = 0x615, // Upper 32 bits of HTimeDelta, HSXLEN=32 only

    VSStatus       = 0x200, // Virtual supervisor status register
    VSIE           = 0x204, // Virtual supervisor interrupt-enable register
    VSTVec         = 0x205, // Virtual supervisor trap handler base address
    VSScratch      = 0x240, // Virtual supervisor scratch register
    VSEPC          = 0x241, // Virtual supervisor exception program register
    VSCause        = 0x242, // Virtual supervisor trap cause
    VSTVal         = 0x243, // Virtual supervisor bad address or instruction
    VSIP           = 0x244, // Virtual supervisor interrupt pending

    VSTimeCmp      = 0x24D, // Virtual supervisor timer register
    VSTimeCmpH     = 0x25D, // Virtual supervisor timer register, RV32 only

    VSATP          = 0x280, // Virtual supervisor address translation and protection

    // Machine-level CSRs

    MVendorID      = 0xF11, // Vendor ID
    MArchID        = 0xF12, // Architecture ID
    MImpID         = 0xF13, // Implementation ID
    MHartID        = 0xF14, // Hardware Thread ID
    MConfigPtr     = 0xF15, // Pointer to configuration data structure

    MStatus        = 0x300, // Machine status register
    MISA           = 0x301, // ISA and extensions
    MEDeleg        = 0x302, // Machine exception delegation register
    MIDeleg        = 0x303, // Machine interrupt delegation register
    MIE            = 0x304, // Machine interrupt-enable register
    MRVec          = 0x305, // Machine trap-handler base address
    MCounterEn     = 0x306, // Machine counter enable
    MStatusH       = 0x310, // Additional machine status register, RV32 only

    MScratch       = 0x340, // Scratch register for machine trap handlers
    MEPC           = 0x341, // Machine exception program counter
    MCause         = 0x342, // Machine trap cause
    MTVal          = 0x343, // Machine bad address or instruction
    MIP            = 0x344, // Machine interrupt pending
    MTInst         = 0x34A, // Machine trap instruction (transformed)
    MTVal2         = 0x34B, // Machine bad guest physical address

    MEnvCfg        = 0x30A, // Machine environment configuration register
    MEnvCfgH       = 0x31A, // Additional machine environment configuration register, RV32 only
    MSecCfg        = 0x747, // Machine security configuration register
    MSecCfgH       = 0x757, // Additional machine security configuration register, RV32 only

    PMPCfg0        = 0x3A0, // Physical memory protection configuration
    PMPCfg1        = 0x3A1, // Physical memory protection configuration, RV32 only
    PMPCfg2        = 0x3A2, // Physical memory protection configuration
    PMPCfg3        = 0x3A3, // Physical memory protection configuration, RV32 only
    PMPCfg4        = 0x3A4, // Physical memory protection configuration
    PMPCfg5        = 0x3A5, // Physical memory protection configuration, RV32 only
    PMPCfg6        = 0x3A6, // Physical memory protection configuration
    PMPCfg7        = 0x3A7, // Physical memory protection configuration, RV32 only
    PMPCfg8        = 0x3A8, // Physical memory protection configuration
    PMPCfg9        = 0x3A9, // Physical memory protection configuration, RV32 only
    PMPCfg10       = 0x3AA, // Physical memory protection configuration
    PMPCfg11       = 0x3AB, // Physical memory protection configuration, RV32 only
    PMPCfg12       = 0x3AC, // Physical memory protection configuration
    PMPCfg13       = 0x3AD, // Physical memory protection configuration, RV32 only
    PMPCfg14       = 0x3AE, // Physical memory protection configuration
    PMPCfg15       = 0x3AF, // Physical memory protection configuration, RV32 only
    PMPAddr0       = 0x3B0, // Physical memory protection address register
    PMPAddr1       = 0x3B1, // Physical memory protection address register
    PMPAddr2       = 0x3B2, // Physical memory protection address register
    PMPAddr3       = 0x3B3, // Physical memory protection address register
    PMPAddr4       = 0x3B4, // Physical memory protection address register
    PMPAddr5       = 0x3B5, // Physical memory protection address register
    PMPAddr6       = 0x3B6, // Physical memory protection address register
    PMPAddr7       = 0x3B7, // Physical memory protection address register
    PMPAddr8       = 0x3B8, // Physical memory protection address register
    PMPAddr9       = 0x3B9, // Physical memory protection address register
    PMPAddr10      = 0x3BA, // Physical memory protection address register
    PMPAddr11      = 0x3BB, // Physical memory protection address register
    PMPAddr12      = 0x3BC, // Physical memory protection address register
    PMPAddr13      = 0x3BD, // Physical memory protection address register
    PMPAddr14      = 0x3BE, // Physical memory protection address register
    PMPAddr15      = 0x3BF, // Physical memory protection address register
    PMPAddr16      = 0x3C0, // Physical memory protection address register
    PMPAddr17      = 0x3C1, // Physical memory protection address register
    PMPAddr18      = 0x3C2, // Physical memory protection address register
    PMPAddr19      = 0x3C3, // Physical memory protection address register
    PMPAddr20      = 0x3C4, // Physical memory protection address register
    PMPAddr21      = 0x3C5, // Physical memory protection address register
    PMPAddr22      = 0x3C6, // Physical memory protection address register
    PMPAddr23      = 0x3C7, // Physical memory protection address register
    PMPAddr24      = 0x3C8, // Physical memory protection address register
    PMPAddr25      = 0x3C9, // Physical memory protection address register
    PMPAddr26      = 0x3CA, // Physical memory protection address register
    PMPAddr27      = 0x3CB, // Physical memory protection address register
    PMPAddr28      = 0x3CC, // Physical memory protection address register
    PMPAddr29      = 0x3CD, // Physical memory protection address register
    PMPAddr30      = 0x3CE, // Physical memory protection address register
    PMPAddr31      = 0x3CF, // Physical memory protection address register
    PMPAddr32      = 0x3D0, // Physical memory protection address register
    PMPAddr33      = 0x3D1, // Physical memory protection address register
    PMPAddr34      = 0x3D2, // Physical memory protection address register
    PMPAddr35      = 0x3D3, // Physical memory protection address register
    PMPAddr36      = 0x3D4, // Physical memory protection address register
    PMPAddr37      = 0x3D5, // Physical memory protection address register
    PMPAddr38      = 0x3D6, // Physical memory protection address register
    PMPAddr39      = 0x3D7, // Physical memory protection address register
    PMPAddr40      = 0x3D8, // Physical memory protection address register
    PMPAddr41      = 0x3D9, // Physical memory protection address register
    PMPAddr42      = 0x3DA, // Physical memory protection address register
    PMPAddr43      = 0x3DB, // Physical memory protection address register
    PMPAddr44      = 0x3DC, // Physical memory protection address register
    PMPAddr45      = 0x3DD, // Physical memory protection address register
    PMPAddr46      = 0x3DE, // Physical memory protection address register
    PMPAddr47      = 0x3DF, // Physical memory protection address register
    PMPAddr48      = 0x3E0, // Physical memory protection address register
    PMPAddr49      = 0x3E1, // Physical memory protection address register
    PMPAddr50      = 0x3E2, // Physical memory protection address register
    PMPAddr51      = 0x3E3, // Physical memory protection address register
    PMPAddr52      = 0x3E4, // Physical memory protection address register
    PMPAddr53      = 0x3E5, // Physical memory protection address register
    PMPAddr54      = 0x3E6, // Physical memory protection address register
    PMPAddr55      = 0x3E7, // Physical memory protection address register
    PMPAddr56      = 0x3E8, // Physical memory protection address register
    PMPAddr57      = 0x3E9, // Physical memory protection address register
    PMPAddr58      = 0x3EA, // Physical memory protection address register
    PMPAddr59      = 0x3EB, // Physical memory protection address register
    PMPAddr60      = 0x3EC, // Physical memory protection address register
    PMPAddr61      = 0x3ED, // Physical memory protection address register
    PMPAddr62      = 0x3EE, // Physical memory protection address register
    PMPAddr63      = 0x3EF, // Physical memory protection address register

    MCycle         = 0xB00, // Machine cycle counter
    MInstRet       = 0xB02, // Machine instructions-retired counter
    MHPMCounter3   = 0xB03, // Machine performance-monitoring counter
    MHPMCounter4   = 0xB04, // Machine performance-monitoring counter
    MHPMCounter5   = 0xB05, // Machine performance-monitoring counter
    MHPMCounter6   = 0xB06, // Machine performance-monitoring counter
    MHPMCounter7   = 0xB07, // Machine performance-monitoring counter
    MHPMCounter8   = 0xB08, // Machine performance-monitoring counter
    MHPMCounter9   = 0xB09, // Machine performance-monitoring counter
    MHPMCounter10  = 0xB0A, // Machine performance-monitoring counter
    MHPMCounter11  = 0xB0B, // Machine performance-monitoring counter
    MHPMCounter12  = 0xB0C, // Machine performance-monitoring counter
    MHPMCounter13  = 0xB0D, // Machine performance-monitoring counter
    MHPMCounter14  = 0xB0E, // Machine performance-monitoring counter
    MHPMCounter15  = 0xB0F, // Machine performance-monitoring counter
    MHPMCounter16  = 0xB10, // Machine performance-monitoring counter
    MHPMCounter17  = 0xB11, // Machine performance-monitoring counter
    MHPMCounter18  = 0xB12, // Machine performance-monitoring counter
    MHPMCounter19  = 0xB13, // Machine performance-monitoring counter
    MHPMCounter20  = 0xB14, // Machine performance-monitoring counter
    MHPMCounter21  = 0xB15, // Machine performance-monitoring counter
    MHPMCounter22  = 0xB16, // Machine performance-monitoring counter
    MHPMCounter23  = 0xB17, // Machine performance-monitoring counter
    MHPMCounter24  = 0xB18, // Machine performance-monitoring counter
    MHPMCounter25  = 0xB19, // Machine performance-monitoring counter
    MHPMCounter26  = 0xB1A, // Machine performance-monitoring counter
    MHPMCounter27  = 0xB1B, // Machine performance-monitoring counter
    MHPMCounter28  = 0xB1C, // Machine performance-monitoring counter
    MHPMCounter29  = 0xB1D, // Machine performance-monitoring counter
    MHPMCounter30  = 0xB1E, // Machine performance-monitoring counter
    MHPMCounter31  = 0xB1F, // Machine performance-monitoring counter

    MCycleH        = 0xB80, // Upper 32 bits ofmcycle, RV32I only
    MInstRetH      = 0xB82, // Upper 32 bits ofminstret, RV32I only

    MHPMCounter3H  = 0xB83, // Upper 32 bits of MHPMCounter3, RV32I only
    MHPMCounter4H  = 0xB84, // Upper 32 bits of MHPMCounter4, RV32I only
    MHPMCounter5H  = 0xB85, // Upper 32 bits of MHPMCounter5, RV32I only
    MHPMCounter6H  = 0xB86, // Upper 32 bits of MHPMCounter6, RV32I only
    MHPMCounter7H  = 0xB87, // Upper 32 bits of MHPMCounter7, RV32I only
    MHPMCounter8H  = 0xB88, // Upper 32 bits of MHPMCounter8, RV32I only
    MHPMCounter9H  = 0xB89, // Upper 32 bits of MHPMCounter9, RV32I only
    MHPMCounter10H = 0xB8A, // Upper 32 bits of MHPMCounter10, RV32I only
    MHPMCounter11H = 0xB8B, // Upper 32 bits of MHPMCounter11, RV32I only
    MHPMCounter12H = 0xB8C, // Upper 32 bits of MHPMCounter12, RV32I only
    MHPMCounter13H = 0xB8D, // Upper 32 bits of MHPMCounter13, RV32I only
    MHPMCounter14H = 0xB8E, // Upper 32 bits of MHPMCounter14, RV32I only
    MHPMCounter15H = 0xB8F, // Upper 32 bits of MHPMCounter15, RV32I only
    MHPMCounter16H = 0xB90, // Upper 32 bits of MHPMCounter16, RV32I only
    MHPMCounter17H = 0xB91, // Upper 32 bits of MHPMCounter17, RV32I only
    MHPMCounter18H = 0xB92, // Upper 32 bits of MHPMCounter18, RV32I only
    MHPMCounter19H = 0xB93, // Upper 32 bits of MHPMCounter19, RV32I only
    MHPMCounter20H = 0xB94, // Upper 32 bits of MHPMCounter20, RV32I only
    MHPMCounter21H = 0xB95, // Upper 32 bits of MHPMCounter21, RV32I only
    MHPMCounter22H = 0xB96, // Upper 32 bits of MHPMCounter22, RV32I only
    MHPMCounter23H = 0xB97, // Upper 32 bits of MHPMCounter23, RV32I only
    MHPMCounter24H = 0xB98, // Upper 32 bits of MHPMCounter24, RV32I only
    MHPMCounter25H = 0xB99, // Upper 32 bits of MHPMCounter25, RV32I only
    MHPMCounter26H = 0xB9A, // Upper 32 bits of MHPMCounter26, RV32I only
    MHPMCounter27H = 0xB9B, // Upper 32 bits of MHPMCounter27, RV32I only
    MHPMCounter28H = 0xB9C, // Upper 32 bits of MHPMCounter28, RV32I only
    MHPMCounter29H = 0xB9D, // Upper 32 bits of MHPMCounter29, RV32I only
    MHPMCounter30H = 0xB9E, // Upper 32 bits of MHPMCounter30, RV32I only
    MHPMCounter31H = 0xB9F, // Upper 32 bits of MHPMCounter31, RV32I only

    MCountInhibit  = 0x320, // Machine counter-inhibit register

    MHPMEvent3     = 0x323, // Machine performance-monitoring event selector
    MHPMEvent4     = 0x324, // Machine performance-monitoring event selector
    MHPMEvent5     = 0x325, // Machine performance-monitoring event selector
    MHPMEvent6     = 0x326, // Machine performance-monitoring event selector
    MHPMEvent7     = 0x327, // Machine performance-monitoring event selector
    MHPMEvent8     = 0x328, // Machine performance-monitoring event selector
    MHPMEvent9     = 0x329, // Machine performance-monitoring event selector
    MHPMEvent10    = 0x32A, // Machine performance-monitoring event selector
    MHPMEvent11    = 0x32B, // Machine performance-monitoring event selector
    MHPMEvent12    = 0x32C, // Machine performance-monitoring event selector
    MHPMEvent13    = 0x32D, // Machine performance-monitoring event selector
    MHPMEvent14    = 0x32E, // Machine performance-monitoring event selector
    MHPMEvent15    = 0x32F, // Machine performance-monitoring event selector
    MHPMEvent16    = 0x330, // Machine performance-monitoring event selector
    MHPMEvent17    = 0x331, // Machine performance-monitoring event selector
    MHPMEvent18    = 0x332, // Machine performance-monitoring event selector
    MHPMEvent19    = 0x333, // Machine performance-monitoring event selector
    MHPMEvent20    = 0x334, // Machine performance-monitoring event selector
    MHPMEvent21    = 0x335, // Machine performance-monitoring event selector
    MHPMEvent22    = 0x336, // Machine performance-monitoring event selector
    MHPMEvent23    = 0x337, // Machine performance-monitoring event selector
    MHPMEvent24    = 0x338, // Machine performance-monitoring event selector
    MHPMEvent25    = 0x339, // Machine performance-monitoring event selector
    MHPMEvent26    = 0x33A, // Machine performance-monitoring event selector
    MHPMEvent27    = 0x33B, // Machine performance-monitoring event selector
    MHPMEvent28    = 0x33C, // Machine performance-monitoring event selector
    MHPMEvent29    = 0x33D, // Machine performance-monitoring event selector
    MHPMEvent30    = 0x33E, // Machine performance-monitoring event selector
    MHPMEvent31    = 0x33F, // Machine performance-monitoring event selector

    TSelect        = 0x7A0, // Debug/Trace trigger register select
    TData1         = 0x7A1, // First Debug/Trace trigger data register
    TData2         = 0x7A2, // Second Debug/Trace trigger data register
    TData3         = 0x7A3, // Third Debug/Trace trigger data register
    MContext       = 0x7A8, // Machine-mode context register

    DCSR           = 0x7B0, // Debug control and status register
    DPC            = 0x7B1, // Debug PC
    DScratch0      = 0x7B2, // Debug scratch register 0
    DScratch1      = 0x7B3, // Debug scratch register 1

    // Scalar Cryptography Entropy Source Extension CSRs

    Seed           = 0x015, // Entropy bit provider (up to 16 bits)

    // Vector Extension CSRs

    VStart         = 0x008, // Vector start position
    VXSat          = 0x009, // Fixed-Point Saturate Flag
    VXRM           = 0x00A, // Fixed-Point Rounding Mode
    VCSR           = 0x00F, // Vector control and status register
    VL             = 0xC20, // Vector length
    VType          = 0xC21, // Vector data type register
    VLenb          = 0xC22, // Vector register length in bytes

    // clang-format on
};

} // namespace biscuit
