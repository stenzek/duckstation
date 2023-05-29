#pragma once

#include <cstdint>

// Source file for anything specific to the RISC-V vector extension.

namespace biscuit {

/// Describes whether or not an instruction should make use of the mask vector.
enum class VecMask : uint32_t {
    Yes = 0,
    No = 1,
};

/// Describes the selected element width.
enum class SEW : uint32_t {
    E8    = 0b000, // 8-bit vector elements
    E16   = 0b001, // 16-bit vector elements
    E32   = 0b010, // 32-bit vector elements
    E64   = 0b011, // 64-bit vector elements
    E128  = 0b100, // 128-bit vector elements
    E256  = 0b101, // 256-bit vector elements
    E512  = 0b110, // 512-bit vector elements
    E1024 = 0b111, // 1024-bit vector elements
};

/// Describes the selected register group multiplier.
enum class LMUL : uint32_t {
    M1  = 0b000, // Group of one vector
    M2  = 0b001, // Groups of two vectors
    M4  = 0b010, // Groups of four vectors
    M8  = 0b011, // Groups of eight vectors
    MF8 = 0b101, // Fractional vector group (1/8)
    MF4 = 0b110, // Fractional vector group (1/4)
    MF2 = 0b111, // Fractional vector group (1/2)
};

/**
 * Describes whether or not vector masks are agnostic.
 *
 * From the RVV spec:
 *
 * When a set is marked undisturbed, the corresponding set of
 * destination elements in a vector register group retain the
 * value they previously held. 
 *
 * When a set is marked agnostic, the corresponding set of destination
 * elements in any vector destination operand can either retain the value
 * they previously held, or are overwritten with 1s.
 *
 * Within a single vector instruction, each destination element can be either
 * left undisturbed or overwritten with 1s, in any combination, and the pattern
 * of undisturbed or overwritten with 1s is not required to be deterministic when
 * the instruction is executed with the same inputs. In addition, except for
 * mask load instructions, any element in the tail of a mask result can also be
 * written with the value the mask-producing operation would have calculated with vl=VLMAX
 */
enum class VMA : uint32_t {
    No,  // Undisturbed
    Yes, // Agnostic
};

/**
 * Describes whether or not vector tail elements are agnostic.
 * 
 * From the RVV spec:
 *
 * When a set is marked undisturbed, the corresponding set of
 * destination elements in a vector register group retain the
 * value they previously held. 
 *
 * When a set is marked agnostic, the corresponding set of destination
 * elements in any vector destination operand can either retain the value
 * they previously held, or are overwritten with 1s.
 *
 * Within a single vector instruction, each destination element can be either
 * left undisturbed or overwritten with 1s, in any combination, and the pattern
 * of undisturbed or overwritten with 1s is not required to be deterministic when
 * the instruction is executed with the same inputs. In addition, except for
 * mask load instructions, any element in the tail of a mask result can also be
 * written with the value the mask-producing operation would have calculated with vl=VLMAX
 */
enum class VTA : uint32_t {
    No,  // Undisturbed
    Yes, // Agnostic
};

} // namespace biscuit
