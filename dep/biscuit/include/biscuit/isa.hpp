#pragma once

#include <cstdint>

// Source file for general values and data structures
// that don't fit a particular criteria related to the ISA.

namespace biscuit {

enum class FenceOrder : uint32_t {
    W = 1, // Write
    R = 2, // Read
    O = 4, // Device Output
    I = 8, // Device Input

    RW = R | W,

    IO = I | O,
    IR = I | R,
    IW = I | W,
    IRW = I | R | W,

    OI = O | I,
    OR = O | R,
    OW = O | W,
    ORW = O | R | W,

    IORW = I | O | R | W,
};

// Atomic ordering
enum class Ordering : uint32_t {
    None = 0,       // None
    RL = 1,         // Release
    AQ = 2,         // Acquire
    AQRL = AQ | RL, // Acquire-Release
};

// Floating-point Rounding Mode
enum class RMode : uint32_t {
    RNE = 0b000, // Round to Nearest, ties to Even
    RTZ = 0b001, // Round towards Zero
    RDN = 0b010, // Round Down (towards negative infinity)
    RUP = 0b011, // Round Up (towards positive infinity)
    RMM = 0b100, // Round to Nearest, ties to Max Magnitude
    DYN = 0b111, // Dynamic Rounding Mode
};

} // namespace biscuit
