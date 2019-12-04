// Copyright 2017, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may
//     be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

extern "C" {
#include <inttypes.h>
#include <stdint.h>
}

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>

#include "utils-vixl.h"
#include "aarch32/constants-aarch32.h"
#include "aarch32/instructions-aarch32.h"
#include "aarch32/operands-aarch32.h"

namespace vixl {
namespace aarch32 {

// Operand

std::ostream& operator<<(std::ostream& os, const Operand& operand) {
  if (operand.IsImmediate()) {
    return os << "#" << operand.GetImmediate();
  }
  if (operand.IsImmediateShiftedRegister()) {
    if ((operand.GetShift().IsLSL() || operand.GetShift().IsROR()) &&
        (operand.GetShiftAmount() == 0)) {
      return os << operand.GetBaseRegister();
    }
    if (operand.GetShift().IsRRX()) {
      return os << operand.GetBaseRegister() << ", rrx";
    }
    return os << operand.GetBaseRegister() << ", " << operand.GetShift() << " #"
              << operand.GetShiftAmount();
  }
  if (operand.IsRegisterShiftedRegister()) {
    return os << operand.GetBaseRegister() << ", " << operand.GetShift() << " "
              << operand.GetShiftRegister();
  }
  VIXL_UNREACHABLE();
  return os;
}

std::ostream& operator<<(std::ostream& os, const NeonImmediate& neon_imm) {
  if (neon_imm.IsDouble()) {
    if (neon_imm.imm_.d_ == 0) {
      if (copysign(1.0, neon_imm.imm_.d_) < 0.0) {
        return os << "#-0.0";
      }
      return os << "#0.0";
    }
    return os << "#" << std::setprecision(9) << neon_imm.imm_.d_;
  }
  if (neon_imm.IsFloat()) {
    if (neon_imm.imm_.f_ == 0) {
      if (copysign(1.0, neon_imm.imm_.d_) < 0.0) return os << "#-0.0";
      return os << "#0.0";
    }
    return os << "#" << std::setprecision(9) << neon_imm.imm_.f_;
  }
  if (neon_imm.IsInteger64()) {
    return os << "#0x" << std::hex << std::setw(16) << std::setfill('0')
              << neon_imm.imm_.u64_ << std::dec;
  }
  return os << "#" << neon_imm.imm_.u32_;
}

// SOperand

std::ostream& operator<<(std::ostream& os, const SOperand& operand) {
  if (operand.IsImmediate()) {
    return os << operand.GetNeonImmediate();
  }
  return os << operand.GetRegister();
}

// DOperand

std::ostream& operator<<(std::ostream& os, const DOperand& operand) {
  if (operand.IsImmediate()) {
    return os << operand.GetNeonImmediate();
  }
  return os << operand.GetRegister();
}

// QOperand

std::ostream& operator<<(std::ostream& os, const QOperand& operand) {
  if (operand.IsImmediate()) {
    return os << operand.GetNeonImmediate();
  }
  return os << operand.GetRegister();
}


ImmediateVbic::ImmediateVbic(DataType dt, const NeonImmediate& neon_imm) {
  if (neon_imm.IsInteger32()) {
    uint32_t immediate = neon_imm.GetImmediate<uint32_t>();
    if (dt.GetValue() == I16) {
      if ((immediate & ~0xff) == 0) {
        SetEncodingValue(0x9);
        SetEncodedImmediate(immediate);
      } else if ((immediate & ~0xff00) == 0) {
        SetEncodingValue(0xb);
        SetEncodedImmediate(immediate >> 8);
      }
    } else if (dt.GetValue() == I32) {
      if ((immediate & ~0xff) == 0) {
        SetEncodingValue(0x1);
        SetEncodedImmediate(immediate);
      } else if ((immediate & ~0xff00) == 0) {
        SetEncodingValue(0x3);
        SetEncodedImmediate(immediate >> 8);
      } else if ((immediate & ~0xff0000) == 0) {
        SetEncodingValue(0x5);
        SetEncodedImmediate(immediate >> 16);
      } else if ((immediate & ~0xff000000) == 0) {
        SetEncodingValue(0x7);
        SetEncodedImmediate(immediate >> 24);
      }
    }
  }
}


DataType ImmediateVbic::DecodeDt(uint32_t cmode) {
  switch (cmode) {
    case 0x1:
    case 0x3:
    case 0x5:
    case 0x7:
      return I32;
    case 0x9:
    case 0xb:
      return I16;
    default:
      break;
  }
  VIXL_UNREACHABLE();
  return kDataTypeValueInvalid;
}


NeonImmediate ImmediateVbic::DecodeImmediate(uint32_t cmode,
                                             uint32_t immediate) {
  switch (cmode) {
    case 0x1:
    case 0x9:
      return immediate;
    case 0x3:
    case 0xb:
      return immediate << 8;
    case 0x5:
      return immediate << 16;
    case 0x7:
      return immediate << 24;
    default:
      break;
  }
  VIXL_UNREACHABLE();
  return 0;
}


ImmediateVmov::ImmediateVmov(DataType dt, const NeonImmediate& neon_imm) {
  if (neon_imm.IsInteger()) {
    switch (dt.GetValue()) {
      case I8:
        if (neon_imm.CanConvert<uint8_t>()) {
          SetEncodingValue(0xe);
          SetEncodedImmediate(neon_imm.GetImmediate<uint8_t>());
        }
        break;
      case I16:
        if (neon_imm.IsInteger32()) {
          uint32_t immediate = neon_imm.GetImmediate<uint32_t>();
          if ((immediate & ~0xff) == 0) {
            SetEncodingValue(0x8);
            SetEncodedImmediate(immediate);
          } else if ((immediate & ~0xff00) == 0) {
            SetEncodingValue(0xa);
            SetEncodedImmediate(immediate >> 8);
          }
        }
        break;
      case I32:
        if (neon_imm.IsInteger32()) {
          uint32_t immediate = neon_imm.GetImmediate<uint32_t>();
          if ((immediate & ~0xff) == 0) {
            SetEncodingValue(0x0);
            SetEncodedImmediate(immediate);
          } else if ((immediate & ~0xff00) == 0) {
            SetEncodingValue(0x2);
            SetEncodedImmediate(immediate >> 8);
          } else if ((immediate & ~0xff0000) == 0) {
            SetEncodingValue(0x4);
            SetEncodedImmediate(immediate >> 16);
          } else if ((immediate & ~0xff000000) == 0) {
            SetEncodingValue(0x6);
            SetEncodedImmediate(immediate >> 24);
          } else if ((immediate & ~0xff00) == 0xff) {
            SetEncodingValue(0xc);
            SetEncodedImmediate(immediate >> 8);
          } else if ((immediate & ~0xff0000) == 0xffff) {
            SetEncodingValue(0xd);
            SetEncodedImmediate(immediate >> 16);
          }
        }
        break;
      case I64: {
        bool is_valid = true;
        uint32_t encoding = 0;
        if (neon_imm.IsInteger32()) {
          uint32_t immediate = neon_imm.GetImmediate<uint32_t>();
          uint32_t mask = 0xff000000;
          for (uint32_t set_bit = 1 << 3; set_bit != 0; set_bit >>= 1) {
            if ((immediate & mask) == mask) {
              encoding |= set_bit;
            } else if ((immediate & mask) != 0) {
              is_valid = false;
              break;
            }
            mask >>= 8;
          }
        } else {
          uint64_t immediate = neon_imm.GetImmediate<uint64_t>();
          uint64_t mask = UINT64_C(0xff) << 56;
          for (uint32_t set_bit = 1 << 7; set_bit != 0; set_bit >>= 1) {
            if ((immediate & mask) == mask) {
              encoding |= set_bit;
            } else if ((immediate & mask) != 0) {
              is_valid = false;
              break;
            }
            mask >>= 8;
          }
        }
        if (is_valid) {
          SetEncodingValue(0x1e);
          SetEncodedImmediate(encoding);
        }
        break;
      }
      default:
        break;
    }
  } else {
    switch (dt.GetValue()) {
      case F32:
        if (neon_imm.IsFloat() || neon_imm.IsDouble()) {
          ImmediateVFP vfp(neon_imm.GetImmediate<float>());
          if (vfp.IsValid()) {
            SetEncodingValue(0xf);
            SetEncodedImmediate(vfp.GetEncodingValue());
          }
        }
        break;
      default:
        break;
    }
  }
}


DataType ImmediateVmov::DecodeDt(uint32_t cmode) {
  switch (cmode & 0xf) {
    case 0x0:
    case 0x2:
    case 0x4:
    case 0x6:
    case 0xc:
    case 0xd:
      return I32;
    case 0x8:
    case 0xa:
      return I16;
    case 0xe:
      return ((cmode & 0x10) == 0) ? I8 : I64;
    case 0xf:
      if ((cmode & 0x10) == 0) return F32;
      break;
    default:
      break;
  }
  VIXL_UNREACHABLE();
  return kDataTypeValueInvalid;
}


NeonImmediate ImmediateVmov::DecodeImmediate(uint32_t cmode,
                                             uint32_t immediate) {
  switch (cmode & 0xf) {
    case 0x8:
    case 0x0:
      return immediate;
    case 0x2:
    case 0xa:
      return immediate << 8;
    case 0x4:
      return immediate << 16;
    case 0x6:
      return immediate << 24;
    case 0xc:
      return (immediate << 8) | 0xff;
    case 0xd:
      return (immediate << 16) | 0xffff;
    case 0xe: {
      if (cmode == 0x1e) {
        uint64_t encoding = 0;
        for (uint32_t set_bit = 1 << 7; set_bit != 0; set_bit >>= 1) {
          encoding <<= 8;
          if ((immediate & set_bit) != 0) {
            encoding |= 0xff;
          }
        }
        return encoding;
      } else {
        return immediate;
      }
    }
    case 0xf: {
      return ImmediateVFP::Decode<float>(immediate);
    }
    default:
      break;
  }
  VIXL_UNREACHABLE();
  return 0;
}


ImmediateVmvn::ImmediateVmvn(DataType dt, const NeonImmediate& neon_imm) {
  if (neon_imm.IsInteger32()) {
    uint32_t immediate = neon_imm.GetImmediate<uint32_t>();
    switch (dt.GetValue()) {
      case I16:
        if ((immediate & ~0xff) == 0) {
          SetEncodingValue(0x8);
          SetEncodedImmediate(immediate);
        } else if ((immediate & ~0xff00) == 0) {
          SetEncodingValue(0xa);
          SetEncodedImmediate(immediate >> 8);
        }
        break;
      case I32:
        if ((immediate & ~0xff) == 0) {
          SetEncodingValue(0x0);
          SetEncodedImmediate(immediate);
        } else if ((immediate & ~0xff00) == 0) {
          SetEncodingValue(0x2);
          SetEncodedImmediate(immediate >> 8);
        } else if ((immediate & ~0xff0000) == 0) {
          SetEncodingValue(0x4);
          SetEncodedImmediate(immediate >> 16);
        } else if ((immediate & ~0xff000000) == 0) {
          SetEncodingValue(0x6);
          SetEncodedImmediate(immediate >> 24);
        } else if ((immediate & ~0xff00) == 0xff) {
          SetEncodingValue(0xc);
          SetEncodedImmediate(immediate >> 8);
        } else if ((immediate & ~0xff0000) == 0xffff) {
          SetEncodingValue(0xd);
          SetEncodedImmediate(immediate >> 16);
        }
        break;
      default:
        break;
    }
  }
}


DataType ImmediateVmvn::DecodeDt(uint32_t cmode) {
  switch (cmode) {
    case 0x0:
    case 0x2:
    case 0x4:
    case 0x6:
    case 0xc:
    case 0xd:
      return I32;
    case 0x8:
    case 0xa:
      return I16;
    default:
      break;
  }
  VIXL_UNREACHABLE();
  return kDataTypeValueInvalid;
}


NeonImmediate ImmediateVmvn::DecodeImmediate(uint32_t cmode,
                                             uint32_t immediate) {
  switch (cmode) {
    case 0x0:
    case 0x8:
      return immediate;
    case 0x2:
    case 0xa:
      return immediate << 8;
    case 0x4:
      return immediate << 16;
    case 0x6:
      return immediate << 24;
    case 0xc:
      return (immediate << 8) | 0xff;
    case 0xd:
      return (immediate << 16) | 0xffff;
    default:
      break;
  }
  VIXL_UNREACHABLE();
  return 0;
}


ImmediateVorr::ImmediateVorr(DataType dt, const NeonImmediate& neon_imm) {
  if (neon_imm.IsInteger32()) {
    uint32_t immediate = neon_imm.GetImmediate<uint32_t>();
    if (dt.GetValue() == I16) {
      if ((immediate & ~0xff) == 0) {
        SetEncodingValue(0x9);
        SetEncodedImmediate(immediate);
      } else if ((immediate & ~0xff00) == 0) {
        SetEncodingValue(0xb);
        SetEncodedImmediate(immediate >> 8);
      }
    } else if (dt.GetValue() == I32) {
      if ((immediate & ~0xff) == 0) {
        SetEncodingValue(0x1);
        SetEncodedImmediate(immediate);
      } else if ((immediate & ~0xff00) == 0) {
        SetEncodingValue(0x3);
        SetEncodedImmediate(immediate >> 8);
      } else if ((immediate & ~0xff0000) == 0) {
        SetEncodingValue(0x5);
        SetEncodedImmediate(immediate >> 16);
      } else if ((immediate & ~0xff000000) == 0) {
        SetEncodingValue(0x7);
        SetEncodedImmediate(immediate >> 24);
      }
    }
  }
}


DataType ImmediateVorr::DecodeDt(uint32_t cmode) {
  switch (cmode) {
    case 0x1:
    case 0x3:
    case 0x5:
    case 0x7:
      return I32;
    case 0x9:
    case 0xb:
      return I16;
    default:
      break;
  }
  VIXL_UNREACHABLE();
  return kDataTypeValueInvalid;
}


NeonImmediate ImmediateVorr::DecodeImmediate(uint32_t cmode,
                                             uint32_t immediate) {
  switch (cmode) {
    case 0x1:
    case 0x9:
      return immediate;
    case 0x3:
    case 0xb:
      return immediate << 8;
    case 0x5:
      return immediate << 16;
    case 0x7:
      return immediate << 24;
    default:
      break;
  }
  VIXL_UNREACHABLE();
  return 0;
}

// MemOperand

std::ostream& operator<<(std::ostream& os, const MemOperand& operand) {
  os << "[" << operand.GetBaseRegister();
  if (operand.GetAddrMode() == PostIndex) {
    os << "]";
    if (operand.IsRegisterOnly()) return os << "!";
  }
  if (operand.IsImmediate()) {
    if ((operand.GetOffsetImmediate() != 0) || operand.GetSign().IsMinus() ||
        ((operand.GetAddrMode() != Offset) && !operand.IsRegisterOnly())) {
      if (operand.GetOffsetImmediate() == 0) {
        os << ", #" << operand.GetSign() << operand.GetOffsetImmediate();
      } else {
        os << ", #" << operand.GetOffsetImmediate();
      }
    }
  } else if (operand.IsPlainRegister()) {
    os << ", " << operand.GetSign() << operand.GetOffsetRegister();
  } else if (operand.IsShiftedRegister()) {
    os << ", " << operand.GetSign() << operand.GetOffsetRegister()
       << ImmediateShiftOperand(operand.GetShift(), operand.GetShiftAmount());
  } else {
    VIXL_UNREACHABLE();
    return os;
  }
  if (operand.GetAddrMode() == Offset) {
    os << "]";
  } else if (operand.GetAddrMode() == PreIndex) {
    os << "]!";
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const AlignedMemOperand& operand) {
  os << "[" << operand.GetBaseRegister() << operand.GetAlignment() << "]";
  if (operand.GetAddrMode() == PostIndex) {
    if (operand.IsPlainRegister()) {
      os << ", " << operand.GetOffsetRegister();
    } else {
      os << "!";
    }
  }
  return os;
}

}  // namespace aarch32
}  // namespace vixl
