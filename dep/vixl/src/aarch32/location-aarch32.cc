// Copyright 2017, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "location-aarch32.h"

#include "assembler-aarch32.h"
#include "macro-assembler-aarch32.h"

namespace vixl {

namespace aarch32 {

bool Location::Needs16BitPadding(int32_t location) const {
  if (!HasForwardReferences()) return false;
  const ForwardRef& last_ref = GetLastForwardReference();
  int32_t min_location_last_ref = last_ref.GetMinLocation();
  VIXL_ASSERT(min_location_last_ref - location <= 2);
  return (min_location_last_ref > location);
}

void Location::ResolveReferences(internal::AssemblerBase* assembler) {
  // Iterate over references and call EncodeLocationFor on each of them.
  for (ForwardRefListIterator it(this); !it.Done(); it.Advance()) {
    const ForwardRef& reference = *it.Current();
    VIXL_ASSERT(reference.LocationIsEncodable(location_));
    int32_t from = reference.GetLocation();
    EncodeLocationFor(assembler, from, reference.op());
  }
  forward_.clear();
}

static bool Is16BitEncoding(uint16_t instr) {
  return instr < (kLowestT32_32Opcode >> 16);
}

void Location::EncodeLocationFor(internal::AssemblerBase* assembler,
                                 int32_t from,
                                 const Location::EmitOperator* encoder) {
  if (encoder->IsUsingT32()) {
    uint16_t* instr_ptr =
        assembler->GetBuffer()->GetOffsetAddress<uint16_t*>(from);
    if (Is16BitEncoding(instr_ptr[0])) {
      // The Encode methods always deals with uint32_t types so we need
      // to explicitly cast it.
      uint32_t instr = static_cast<uint32_t>(instr_ptr[0]);
      instr = encoder->Encode(instr, from, this);
      // The Encode method should not ever set the top 16 bits.
      VIXL_ASSERT((instr & ~0xffff) == 0);
      instr_ptr[0] = static_cast<uint16_t>(instr);
    } else {
      uint32_t instr =
          instr_ptr[1] | (static_cast<uint32_t>(instr_ptr[0]) << 16);
      instr = encoder->Encode(instr, from, this);
      instr_ptr[0] = static_cast<uint16_t>(instr >> 16);
      instr_ptr[1] = static_cast<uint16_t>(instr);
    }
  } else {
    uint32_t* instr_ptr =
        assembler->GetBuffer()->GetOffsetAddress<uint32_t*>(from);
    instr_ptr[0] = encoder->Encode(instr_ptr[0], from, this);
  }
}

void Location::AddForwardRef(int32_t instr_location,
                             const EmitOperator& op,
                             const ReferenceInfo* info) {
  VIXL_ASSERT(referenced_);
  int32_t from = instr_location + (op.IsUsingT32() ? kT32PcDelta : kA32PcDelta);
  if (info->pc_needs_aligning == ReferenceInfo::kAlignPc)
    from = AlignDown(from, 4);
  int32_t min_object_location = from + info->min_offset;
  int32_t max_object_location = from + info->max_offset;
  forward_.insert(ForwardRef(&op,
                             instr_location,
                             info->size,
                             min_object_location,
                             max_object_location,
                             info->alignment));
}

int Location::GetMaxAlignment() const {
  int max_alignment = GetPoolObjectAlignment();
  for (ForwardRefListIterator it(const_cast<Location*>(this)); !it.Done();
       it.Advance()) {
    const ForwardRef& reference = *it.Current();
    if (reference.GetAlignment() > max_alignment)
      max_alignment = reference.GetAlignment();
  }
  return max_alignment;
}

int Location::GetMinLocation() const {
  int32_t min_location = 0;
  for (ForwardRefListIterator it(const_cast<Location*>(this)); !it.Done();
       it.Advance()) {
    const ForwardRef& reference = *it.Current();
    if (reference.GetMinLocation() > min_location)
      min_location = reference.GetMinLocation();
  }
  return min_location;
}

void Label::UpdatePoolObject(PoolObject<int32_t>* object) {
  VIXL_ASSERT(forward_.size() == 1);
  const ForwardRef& reference = forward_.Front();
  object->Update(reference.GetMinLocation(),
                 reference.GetMaxLocation(),
                 reference.GetAlignment());
}

void Label::EmitPoolObject(MacroAssemblerInterface* masm) {
  MacroAssembler* macro_assembler = static_cast<MacroAssembler*>(masm);

  // Add a new branch to this label.
  macro_assembler->GetBuffer()->EnsureSpaceFor(kMaxInstructionSizeInBytes);
  ExactAssemblyScopeWithoutPoolsCheck guard(macro_assembler,
                                            kMaxInstructionSizeInBytes,
                                            ExactAssemblyScope::kMaximumSize);
  macro_assembler->b(this);
}

void RawLiteral::EmitPoolObject(MacroAssemblerInterface* masm) {
  Assembler* assembler = static_cast<Assembler*>(masm->AsAssemblerBase());

  assembler->GetBuffer()->EnsureSpaceFor(GetSize());
  assembler->GetBuffer()->EmitData(GetDataAddress(), GetSize());
}
}
}
