#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include <biscuit/assert.hpp>

namespace biscuit {

/**
 * An arbitrarily sized buffer that code is written into.
 *
 * Also contains other member functions for manipulating
 * the data within the code buffer.
 */
class CodeBuffer {
public:
    // Default capacity of 4KB.
    static constexpr size_t default_capacity = 4096;

    /**
     * Constructor
     *
     * @param capacity The initial capacity of the code buffer in bytes.
     */
    explicit CodeBuffer(size_t capacity = default_capacity);

    /**
     * Constructor
     *
     * @param buffer   A non-null pointer to an allocated buffer of size `capacity`.
     * @param capacity The capacity of the memory pointed to by `buffer`.
     *
     * @pre The given memory buffer must not be null.
     * @pre The given memory buffer must be at minimum `capacity` bytes in size.
     *
     * @note The caller is responsible for managing the lifetime of the given memory.
     *       CodeBuffer will *not* free the memory once it goes out of scope.
     */
    explicit CodeBuffer(uint8_t* buffer, size_t capacity);

    // Copy constructor and assignment is deleted in order to prevent unintentional memory leaks.
    CodeBuffer(const CodeBuffer&) = delete;
    CodeBuffer& operator=(const CodeBuffer&) = delete;

    // Move constructing or moving the buffer in general is allowed, as it's a transfer of control.
    CodeBuffer(CodeBuffer&& other) noexcept;
    CodeBuffer& operator=(CodeBuffer&& other) noexcept;

    /**
     * Destructor
     *
     * If a custom memory buffer is not given to the code buffer,
     * then the code buffer will automatically free any memory
     * it had allocated in order to be able to emit code.
     */
    ~CodeBuffer() noexcept;

    /// Returns whether or not the memory is managed by the code buffer.
    [[nodiscard]] bool IsManaged() const noexcept { return m_is_managed; }

    /// Retrieves the current cursor position within the buffer.
    [[nodiscard]] ptrdiff_t GetCursorOffset() const noexcept {
        return m_cursor - m_buffer;
    }

    /// Retrieves the current address of the cursor within the buffer.
    [[nodiscard]] uintptr_t GetCursorAddress() const noexcept {
        return GetOffsetAddress(GetCursorOffset());
    }

    /// Retrieves the cursor pointer
    [[nodiscard]] uint8_t* GetCursorPointer() noexcept {
        return GetOffsetPointer(GetCursorOffset());
    }

    /// Retrieves the cursor pointer
    [[nodiscard]] const uint8_t* GetCursorPointer() const noexcept {
        return GetOffsetPointer(GetCursorOffset());
    }

    /// Retrieves the address of an arbitrary offset within the buffer.
    [[nodiscard]] uintptr_t GetOffsetAddress(ptrdiff_t offset) const noexcept {
        return reinterpret_cast<uintptr_t>(GetOffsetPointer(offset));
    }

    /// Retrieves the pointer to an arbitrary location within the buffer.
    [[nodiscard]] uint8_t* GetOffsetPointer(ptrdiff_t offset) noexcept {
        BISCUIT_ASSERT(offset >= 0 && offset <= GetCursorOffset());
        return m_buffer + offset;
    }

    /// Retrieves the pointer to an arbitrary location within the buffer.
    [[nodiscard]] const uint8_t* GetOffsetPointer(ptrdiff_t offset) const noexcept {
        BISCUIT_ASSERT(offset >= 0 && offset <= GetCursorOffset());
        return m_buffer + offset;
    }

    /**
     * Allows rewinding of the code buffer cursor.
     *
     * @param offset The offset to rewind the cursor by.
     *
     * @note If no offset is provided, then this function rewinds the
     *       cursor to the beginning of the buffer.
     *
     * @note The offset may not be larger than the current cursor offset
     *       and may not be less than the current buffer starting address.
     */
    void RewindCursor(ptrdiff_t offset = 0) noexcept {
        auto* rewound = m_buffer + offset;
        BISCUIT_ASSERT(m_buffer <= rewound && rewound <= m_cursor);
        m_cursor = rewound;
    }

    /**
     * Whether or not the underlying buffer has enough room for the
     * given number of bytes.
     *
     * @param num_bytes The number of bytes to store in the buffer.
     */
    [[nodiscard]] bool HasSpaceFor(size_t num_bytes) const noexcept {
        return GetRemainingBytes() >= num_bytes;
    }

    /// Returns the size of the data written to the buffer in bytes.
    [[nodiscard]] size_t GetSizeInBytes() const noexcept {
        EnsureBufferRange();
        return static_cast<size_t>(m_cursor - m_buffer);
    }

    /// Returns the total number of remaining bytes in the buffer.
    [[nodiscard]] size_t GetRemainingBytes() const noexcept {
        EnsureBufferRange();
        return static_cast<size_t>((m_buffer + m_capacity) - m_cursor);
    }

    /**
     * Grows the underlying memory of the code buffer
     *
     * @param new_capacity The new capacity of the code buffer in bytes.
     *
     * @pre The underlying memory of the code buffer *must* be managed
     *      by the code buffer itself. Attempts to grow the buffer
     *      with memory that is not managed by it will result in
     *      an assertion being hit.
     *
     * @note Calling this with a new capacity that is less than or equal
     *       to the current capacity of the buffer will result in
     *       this function doing nothing.
     */
    void Grow(size_t new_capacity);

    /**
     * Emits a given value into the code buffer.
     *
     * @param value The value to emit into the code buffer.
     * @tparam T    A trivially-copyable type.
     */
    template <typename T>
    void Emit(T value) noexcept {
        static_assert(std::is_trivially_copyable_v<T>,
                      "It's undefined behavior to memcpy a non-trivially-copyable type.");
        BISCUIT_ASSERT(HasSpaceFor(sizeof(T)));

        std::memcpy(m_cursor, &value, sizeof(T));
        m_cursor += sizeof(T);
    }

    /// Emits a 16-bit value into the code buffer.
    void Emit16(uint32_t value) noexcept {
        Emit(static_cast<uint16_t>(value));
    }

    /// Emits a 32-bit value into the code buffer.
    void Emit32(uint32_t value) noexcept {
        Emit(value);
    }

    /**
     * Sets the internal code buffer to be executable.
     *
     * @note This will make the contained region of memory non-writable
     *       to satisfy operating under W^X contexts. To make the
     *       region writable again, use SetWritable().
     */
    void SetExecutable();

    /**
     * Sets the internal code buffer to be writable
     *
     * @note This will make the contained region of memory non-executable
     *       to satisfy operating under W^X contexts. To make the region
     *       executable again, use SetExecutable().
     */
    void SetWritable();

private:
    void EnsureBufferRange() const noexcept {
        BISCUIT_ASSERT(m_cursor >= m_buffer && m_cursor <= m_buffer + m_capacity);
    }

    uint8_t* m_buffer = nullptr;
    uint8_t* m_cursor = nullptr;
    size_t m_capacity = 0;
    bool m_is_managed = false;
};

} // namespace biscuit
