#pragma once

#include <cstdint>

namespace biscuit {

/**
 * Generic abstraction around a register.
 *
 * This is less bug-prone than using raw primitive sizes
 * in opcode emitter functions, since it provides stronger typing.
 */
class Register {
public:
    constexpr Register() noexcept = default;

    /// Gets the index for this register.
    [[nodiscard]] constexpr uint32_t Index() const noexcept {
        return m_index;
    }

    /// Determines whether or not this register is a general-purpose register.
    [[nodiscard]] constexpr bool IsGPR() const noexcept {
        return m_type == Type::GPR;
    }

    /// Determines whether or not this register is a floating-point register.
    [[nodiscard]] constexpr bool IsFPR() const noexcept {
        return m_type == Type::FPR;
    }

    /// Determines whether or not this register is a vector register.
    [[nodiscard]] constexpr bool IsVector() const noexcept {
        return m_type == Type::Vector;
    }

protected:
    enum class Type {
        GPR,    // General purpose register
        FPR,    // Floating-point register
        Vector, // Vector register
    };

    constexpr Register(uint32_t index, Type type) noexcept
        : m_index{index}, m_type{type} {}

private:
    uint32_t m_index{};
    Type m_type{};
};

/// General purpose register.
class GPR final : public Register {
public:
    constexpr GPR() noexcept : Register{0, Type::GPR} {}
    constexpr explicit GPR(uint32_t index) noexcept : Register{index, Type::GPR} {}

    friend constexpr bool operator==(GPR lhs, GPR rhs) noexcept {
        return lhs.Index() == rhs.Index();
    }
    friend constexpr bool operator!=(GPR lhs, GPR rhs) noexcept {
        return !operator==(lhs, rhs);
    }
};

/// Floating point register.
class FPR final : public Register {
public:
    constexpr FPR() noexcept : Register{0, Type::FPR} {}
    constexpr explicit FPR(uint32_t index) noexcept : Register{index, Type::FPR} {}

    friend constexpr bool operator==(FPR lhs, FPR rhs) noexcept {
        return lhs.Index() == rhs.Index();
    }
    friend constexpr bool operator!=(FPR lhs, FPR rhs) noexcept {
        return !operator==(lhs, rhs);
    }
};

/// Vector register.
class Vec final : public Register {
public:
    constexpr Vec() noexcept : Register{0, Type::Vector} {}
    constexpr explicit Vec(uint32_t index) noexcept : Register{index, Type::Vector} {}

    friend constexpr bool operator==(Vec lhs, Vec rhs) noexcept {
        return lhs.Index() == rhs.Index();
    }
    friend constexpr bool operator!=(Vec lhs, Vec rhs) noexcept {
        return !operator==(lhs, rhs);
    }
};

// General-purpose Registers

constexpr GPR x0{0};
constexpr GPR x1{1};
constexpr GPR x2{2};
constexpr GPR x3{3};
constexpr GPR x4{4};
constexpr GPR x5{5};
constexpr GPR x6{6};
constexpr GPR x7{7};
constexpr GPR x8{8};
constexpr GPR x9{9};
constexpr GPR x10{10};
constexpr GPR x11{11};
constexpr GPR x12{12};
constexpr GPR x13{13};
constexpr GPR x14{14};
constexpr GPR x15{15};
constexpr GPR x16{16};
constexpr GPR x17{17};
constexpr GPR x18{18};
constexpr GPR x19{19};
constexpr GPR x20{20};
constexpr GPR x21{21};
constexpr GPR x22{22};
constexpr GPR x23{23};
constexpr GPR x24{24};
constexpr GPR x25{25};
constexpr GPR x26{26};
constexpr GPR x27{27};
constexpr GPR x28{28};
constexpr GPR x29{29};
constexpr GPR x30{30};
constexpr GPR x31{31};

// Symbolic General-purpose Register Names

constexpr GPR zero{x0};

constexpr GPR ra{x1};
constexpr GPR sp{x2};
constexpr GPR gp{x3};
constexpr GPR tp{x4};
constexpr GPR fp{x8};

constexpr GPR a0{x10};
constexpr GPR a1{x11};
constexpr GPR a2{x12};
constexpr GPR a3{x13};
constexpr GPR a4{x14};
constexpr GPR a5{x15};
constexpr GPR a6{x16};
constexpr GPR a7{x17};

constexpr GPR s0{x8};
constexpr GPR s1{x9};
constexpr GPR s2{x18};
constexpr GPR s3{x19};
constexpr GPR s4{x20};
constexpr GPR s5{x21};
constexpr GPR s6{x22};
constexpr GPR s7{x23};
constexpr GPR s8{x24};
constexpr GPR s9{x25};
constexpr GPR s10{x26};
constexpr GPR s11{x27};

constexpr GPR t0{x5};
constexpr GPR t1{x6};
constexpr GPR t2{x7};
constexpr GPR t3{x28};
constexpr GPR t4{x29};
constexpr GPR t5{x30};
constexpr GPR t6{x31};

// Floating-point registers

constexpr FPR f0{0};
constexpr FPR f1{1};
constexpr FPR f2{2};
constexpr FPR f3{3};
constexpr FPR f4{4};
constexpr FPR f5{5};
constexpr FPR f6{6};
constexpr FPR f7{7};
constexpr FPR f8{8};
constexpr FPR f9{9};
constexpr FPR f10{10};
constexpr FPR f11{11};
constexpr FPR f12{12};
constexpr FPR f13{13};
constexpr FPR f14{14};
constexpr FPR f15{15};
constexpr FPR f16{16};
constexpr FPR f17{17};
constexpr FPR f18{18};
constexpr FPR f19{19};
constexpr FPR f20{20};
constexpr FPR f21{21};
constexpr FPR f22{22};
constexpr FPR f23{23};
constexpr FPR f24{24};
constexpr FPR f25{25};
constexpr FPR f26{26};
constexpr FPR f27{27};
constexpr FPR f28{28};
constexpr FPR f29{29};
constexpr FPR f30{30};
constexpr FPR f31{31};

// Symbolic Floating-point Register Names

constexpr FPR fa0{f10};
constexpr FPR fa1{f11};
constexpr FPR fa2{f12};
constexpr FPR fa3{f13};
constexpr FPR fa4{f14};
constexpr FPR fa5{f15};
constexpr FPR fa6{f16};
constexpr FPR fa7{f17};

constexpr FPR ft0{f0};
constexpr FPR ft1{f1};
constexpr FPR ft2{f2};
constexpr FPR ft3{f3};
constexpr FPR ft4{f4};
constexpr FPR ft5{f5};
constexpr FPR ft6{f6};
constexpr FPR ft7{f7};
constexpr FPR ft8{f28};
constexpr FPR ft9{f29};
constexpr FPR ft10{f30};
constexpr FPR ft11{f31};

constexpr FPR fs0{f8};
constexpr FPR fs1{f9};
constexpr FPR fs2{f18};
constexpr FPR fs3{f19};
constexpr FPR fs4{f20};
constexpr FPR fs5{f21};
constexpr FPR fs6{f22};
constexpr FPR fs7{f23};
constexpr FPR fs8{f24};
constexpr FPR fs9{f25};
constexpr FPR fs10{f26};
constexpr FPR fs11{f27};

// Vector registers (V extension)

constexpr Vec v0{0};
constexpr Vec v1{1};
constexpr Vec v2{2};
constexpr Vec v3{3};
constexpr Vec v4{4};
constexpr Vec v5{5};
constexpr Vec v6{6};
constexpr Vec v7{7};
constexpr Vec v8{8};
constexpr Vec v9{9};
constexpr Vec v10{10};
constexpr Vec v11{11};
constexpr Vec v12{12};
constexpr Vec v13{13};
constexpr Vec v14{14};
constexpr Vec v15{15};
constexpr Vec v16{16};
constexpr Vec v17{17};
constexpr Vec v18{18};
constexpr Vec v19{19};
constexpr Vec v20{20};
constexpr Vec v21{21};
constexpr Vec v22{22};
constexpr Vec v23{23};
constexpr Vec v24{24};
constexpr Vec v25{25};
constexpr Vec v26{26};
constexpr Vec v27{27};
constexpr Vec v28{28};
constexpr Vec v29{29};
constexpr Vec v30{30};
constexpr Vec v31{31};

} // namespace biscuit
