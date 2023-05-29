#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <biscuit/assert.hpp>

namespace biscuit {

/**
 * A label is a representation of an address that can be used with branch and jump instructions.
 *
 * Labels do not need to be bound to a location immediately. A label can be created
 * to provide branches with a tentative, undecided location that is then bound
 * at a later point in time.
 *
 * @note Any label that is created, is used with a branch instruction,
 *       but is *not* bound to a location (via Bind() in the assembler)
 *       will result in an assertion being invoked when the label instance's
 *       destructor is executed.
 *
 * @note A label may only be bound to one location. Any attempt to rebind
 *       a label that is already bound will result in an assertion being
 *       invoked.
 *
 * @par
 * An example of binding a label:
 *
 * @code{.cpp}
 * Assembler as{...};
 * Label label;
 *
 * as.BNE(x2, x3, &label); // Use the label
 * as.ADD(x7, x8, x9);
 * as.XOR(x7, x10, x12);
 * as.Bind(&label);        // Bind the label to a location
 * @endcode
 */
class Label {
public:
    using Location = std::optional<ptrdiff_t>;
    using LocationOffset = Location::value_type;

    /**
     * Default constructor.
     *
     * This constructor results in a label being constructed that is not
     * bound to a particular location yet.
     */
    explicit Label() = default;

    /// Destructor
    ~Label() noexcept {
        // It's a logic bug if something references a label and hasn't been handled.
        //
        // This is usually indicative of a scenario where a label is referenced but
        // hasn't been bound to a location.
        //
        BISCUIT_ASSERT(IsResolved());
    }

    // We disable copying of labels, as this doesn't really make sense to do.
    // It also presents a problem. When labels are being resolved, if we have
    // two labels pointing to the same place, resolving the links to this address
    // are going to clobber each other N times for however many copies of the label
    // exist.
    //
    // This isn't a particularly major problem, since the resolving will still result
    // in the same end result, but it does make it annoying to think about label interactions
    // moving forward. Thus, I choose to simply not think about it at all!
    //
    Label(const Label&) = delete;
    Label& operator=(const Label&) = delete;

    // Moving labels on the other hand is totally fine, this is just pushing data around
    // to another label while invalidating the label having it's data "stolen".
    Label(Label&&) noexcept = default;
    Label& operator=(Label&&) noexcept = default;

    /**
     * Determines whether or not this label instance has a location assigned to it.
     *
     * A label is considered bound if it has an assigned location.
     */
    [[nodiscard]] bool IsBound() const noexcept {
        return m_location.has_value();
    }

    /**
     * Determines whether or not this label is resolved.
     *
     * A label is considered resolved when all referencing offsets have been handled.
     */
    [[nodiscard]] bool IsResolved() const noexcept {
        return m_offsets.empty();
    }

    /**
     * Determines whether or not this label is unresolved.
     *
     * A label is considered unresolved if it still has any unhandled referencing offsets.
     */
    [[nodiscard]] bool IsUnresolved() const noexcept {
        return !IsResolved();
    }

    /**
     * Retrieves the location for this label.
     *
     * @note If the returned location is empty, then this label has not been assigned
     *       a location yet.
     */
    [[nodiscard]] Location GetLocation() const noexcept {
        return m_location;
    }

private:
    // A label instance is inherently bound to the assembler it's
    // used with, as the offsets within the label set depend on
    // said assemblers code buffer.
    friend class Assembler;

    /**
     * Binds a label to the given location.
     *
     * @param offset The instruction offset to bind this label to.
     *
     * @pre The label must not have already been bound to a previous location.
     *      Attempting to rebind a label is typically, in almost all scenarios,
     *      the source of bugs.
     *      Attempting to rebind an already bound label will result in an assertion
     *      being triggered.
     */
    void Bind(LocationOffset offset) noexcept {
        BISCUIT_ASSERT(!IsBound());
        m_location = offset;
    }

    /**
     * Marks the given address as dependent on this label.
     *
     * This is used in scenarios where a label exists, but has not yet been
     * bound to a location yet. It's important to track these addresses,
     * as we'll need to patch the dependent branch instructions with the
     * proper offset once the label is finally bound by the assembler.
     *
     * During label binding, the offset will be calculated and inserted
     * into dependent instructions.
     */
    void AddOffset(LocationOffset offset) {
        // If a label is already bound to a location, then offset tracking
        // isn't necessary. Tripping this assert means we have a bug somewhere.
        BISCUIT_ASSERT(!IsBound());
        BISCUIT_ASSERT(IsNewOffset(offset));

        m_offsets.insert(offset);
    }

    // Clears all the underlying offsets for this label.
    void ClearOffsets() noexcept {
        m_offsets.clear();
    }

    // Determines whether or not this address has already been added before.
    [[nodiscard]] bool IsNewOffset(LocationOffset offset) const noexcept {
        return m_offsets.find(offset) == m_offsets.cend();
    }

    std::set<LocationOffset> m_offsets;
    Location m_location;
};

} // namespace biscuit
