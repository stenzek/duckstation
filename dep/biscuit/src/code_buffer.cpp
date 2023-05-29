#include <biscuit/assert.hpp>
#include <biscuit/code_buffer.hpp>

#include <cstring>
#include <utility>

#ifdef BISCUIT_CODE_BUFFER_MMAP
#include <sys/mman.h>
#endif

namespace biscuit {

CodeBuffer::CodeBuffer(size_t capacity)
    : m_capacity{capacity}, m_is_managed{true} {
    if (capacity == 0) {
        return;
    }

#ifdef BISCUIT_CODE_BUFFER_MMAP
    m_buffer = static_cast<uint8_t*>(mmap(nullptr, capacity,
                                          PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS,
                                          -1, 0));
    BISCUIT_ASSERT(m_buffer != nullptr);
#else
    m_buffer = new uint8_t[capacity]();
#endif

    m_cursor = m_buffer;
}

CodeBuffer::CodeBuffer(uint8_t* buffer, size_t capacity)
    : m_buffer{buffer}, m_cursor{buffer}, m_capacity{capacity} {
    BISCUIT_ASSERT(buffer != nullptr);
}

CodeBuffer::CodeBuffer(CodeBuffer&& other) noexcept
    : m_buffer{std::exchange(other.m_buffer, nullptr)}
    , m_cursor{std::exchange(other.m_cursor, nullptr)}
    , m_capacity{std::exchange(other.m_capacity, size_t{0})}
    , m_is_managed{std::exchange(other.m_is_managed, false)} {}

CodeBuffer& CodeBuffer::operator=(CodeBuffer&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    std::swap(m_buffer, other.m_buffer);
    std::swap(m_cursor, other.m_cursor);
    std::swap(m_capacity, other.m_capacity);
    std::swap(m_is_managed, other.m_is_managed);
    return *this;
}

CodeBuffer::~CodeBuffer() noexcept {
    if (!m_is_managed) {
        return;
    }

#ifdef BISCUIT_CODE_BUFFER_MMAP
    munmap(m_buffer, m_capacity);
#else
    delete[] m_buffer;
#endif
}

void CodeBuffer::Grow(size_t new_capacity) {
    BISCUIT_ASSERT(IsManaged());

    // No-op, just return.
    if (new_capacity <= m_capacity) {
        return;
    }

    const auto cursor_offset = GetCursorOffset();

#ifdef BISCUIT_CODE_BUFFER_MMAP
    auto* new_buffer = static_cast<uint8_t*>(mremap(m_buffer, m_capacity, new_capacity, MREMAP_MAYMOVE));
    BISCUIT_ASSERT(new_buffer != nullptr);
#else
    auto* new_buffer = new uint8_t[new_capacity]();
    std::memcpy(new_buffer, m_buffer, m_capacity);
    delete[] m_buffer;
#endif

    m_buffer = new_buffer;
    m_capacity = new_capacity;
    m_cursor = m_buffer + cursor_offset;
}

void CodeBuffer::SetExecutable() {
#ifdef BISCUIT_CODE_BUFFER_MMAP
    const auto result = mprotect(m_buffer, m_capacity, PROT_READ | PROT_EXEC);
    BISCUIT_ASSERT(result == 0);
#else
    // Unimplemented/Unnecessary for new
    BISCUIT_ASSERT(false);
#endif
}

void CodeBuffer::SetWritable() {
#ifdef BISCUIT_CODE_BUFFER_MMAP
    const auto result = mprotect(m_buffer, m_capacity, PROT_READ | PROT_WRITE);
    BISCUIT_ASSERT(result == 0);
#else
    // Unimplemented/Unnecessary for new
    BISCUIT_ASSERT(false);
#endif
}

} // namespace biscuit
