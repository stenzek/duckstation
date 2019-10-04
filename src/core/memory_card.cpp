#include "memory_card.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
Log_SetChannel(MemoryCard);

MemoryCard::MemoryCard()
{
  m_FLAG.no_write_yet = true;
  Format();
}

MemoryCard::~MemoryCard() = default;

void MemoryCard::Reset()
{
  ResetTransferState();
}

bool MemoryCard::DoState(StateWrapper& sw)
{
  sw.Do(&m_state);
  sw.Do(&m_address);
  sw.Do(&m_sector_offset);
  sw.Do(&m_checksum);
  sw.Do(&m_last_byte);
  sw.Do(&m_data);

  return !sw.HasError();
}

void MemoryCard::ResetTransferState()
{
  m_state = State::Idle;
  m_address = 0;
  m_sector_offset = 0;
  m_checksum = 0;
  m_last_byte = 0;
}

bool MemoryCard::Transfer(const u8 data_in, u8* data_out)
{
  bool ack = false;
  const State old_state = m_state;

  switch (m_state)
  {

#define FIXED_REPLY_STATE(state, reply, ack_value, next_state)                                                         \
  case state:                                                                                                          \
  {                                                                                                                    \
    *data_out = reply;                                                                                                 \
    ack = ack_value;                                                                                                   \
    m_state = next_state;                                                                                              \
  }                                                                                                                    \
  break;

#define ADDRESS_STATE_MSB(state, next_state)                                                                           \
  case state:                                                                                                          \
  {                                                                                                                    \
    *data_out = 0x00;                                                                                                  \
    ack = true;                                                                                                        \
    m_address = ((m_address & u16(0x00FF)) | (ZeroExtend16(data_in) << 8)) & 0x3FF;                                    \
    m_state = next_state;                                                                                              \
  }                                                                                                                    \
  break;

#define ADDRESS_STATE_LSB(state, next_state)                                                                           \
  case state:                                                                                                          \
  {                                                                                                                    \
    *data_out = m_last_byte;                                                                                           \
    ack = true;                                                                                                        \
    m_address = ((m_address & u16(0xFF00)) | ZeroExtend16(data_in)) & 0x3FF;                                           \
    m_sector_offset = 0;                                                                                               \
    m_state = next_state;                                                                                              \
  }                                                                                                                    \
  break;

    // read state

    FIXED_REPLY_STATE(State::ReadCardID1, 0x5A, true, State::ReadCardID2);
    FIXED_REPLY_STATE(State::ReadCardID2, 0x5D, true, State::ReadAddressMSB);
    ADDRESS_STATE_MSB(State::ReadAddressMSB, State::ReadAddressLSB);
    ADDRESS_STATE_LSB(State::ReadAddressLSB, State::ReadACK1);
    FIXED_REPLY_STATE(State::ReadACK1, 0x5C, true, State::ReadACK2);
    FIXED_REPLY_STATE(State::ReadACK2, 0x5D, true, State::ReadConfirmAddressMSB);
    FIXED_REPLY_STATE(State::ReadConfirmAddressMSB, Truncate8(m_address >> 8), true, State::ReadConfirmAddressLSB);
    FIXED_REPLY_STATE(State::ReadConfirmAddressLSB, Truncate8(m_address), true, State::ReadData);

    case State::ReadData:
    {
      const u8 bits = m_data[ZeroExtend32(m_address) * SECTOR_SIZE + m_sector_offset];
      if (m_sector_offset == 0)
      {
        Log_DevPrintf("Reading memory card sector %u", ZeroExtend32(m_address));
        m_checksum = Truncate8(m_address >> 8) ^ Truncate8(m_address) ^ bits;
      }
      else
      {
        m_checksum ^= bits;
      }

      *data_out = bits;
      ack = true;

      m_sector_offset++;
      if (m_sector_offset == SECTOR_SIZE)
      {
        m_state = State::ReadChecksum;
        m_sector_offset = 0;
      }
    }
    break;

      FIXED_REPLY_STATE(State::ReadChecksum, m_checksum, true, State::ReadEnd);
      FIXED_REPLY_STATE(State::ReadEnd, 0x47, true, State::Idle);

      // write state

      FIXED_REPLY_STATE(State::WriteCardID1, 0x5A, true, State::WriteCardID2);
      FIXED_REPLY_STATE(State::WriteCardID2, 0x5D, true, State::WriteAddressMSB);
      ADDRESS_STATE_MSB(State::WriteAddressMSB, State::WriteAddressLSB);
      ADDRESS_STATE_LSB(State::WriteAddressLSB, State::WriteData);

    case State::WriteData:
    {
      if (m_sector_offset == 0)
      {
        Log_DevPrintf("Writing memory card sector %u", ZeroExtend32(m_address));
        m_checksum = Truncate8(m_address >> 8) ^ Truncate8(m_address) ^ data_in;
        m_FLAG.no_write_yet = false;
      }
      else
      {
        m_checksum ^= data_in;
      }

      m_data[ZeroExtend32(m_address) * SECTOR_SIZE + m_sector_offset] = data_in;
      *data_out = m_last_byte;
      ack = true;

      m_sector_offset++;
      if (m_sector_offset == SECTOR_SIZE)
      {
        m_state = State::WriteChecksum;
        m_sector_offset = 0;
      }
    }
    break;

      FIXED_REPLY_STATE(State::WriteChecksum, m_checksum, true, State::WriteACK1);
      FIXED_REPLY_STATE(State::WriteACK1, 0x5C, true, State::WriteACK2);
      FIXED_REPLY_STATE(State::WriteACK2, 0x5D, true, State::WriteEnd);
      FIXED_REPLY_STATE(State::WriteEnd, 0x47, true, State::Idle);

      // new command
    case State::Idle:
    {
      // select device
      if (data_in == 0x81)
      {
        *data_out = 0xFF;
        ack = true;
        m_state = State::Command;
      }
    }
    break;

    case State::Command:
    {
      switch (data_in)
      {
        case 0x52: // read data
        {
          *data_out = m_FLAG.bits;
          ack = true;
          m_state = State::ReadCardID1;
        }
        break;

        case 0x57: // write data
        {
          *data_out = m_FLAG.bits;
          ack = true;
          m_state = State::WriteCardID1;
        }
        break;

        case 0x53: // get id
        {
          Panic("implement me");
        }
        break;

        default:
        {
          Log_ErrorPrintf("Invalid command 0x%02X", ZeroExtend32(data_in));
          *data_out = m_FLAG.bits;
          ack = false;
          m_state = State::Idle;
        }
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }

  Log_DebugPrintf("Transfer, old_state=%u, new_state=%u, data_in=0x%02X, data_out=0x%02X, ack=%s",
                  static_cast<u32>(old_state), static_cast<u32>(m_state), data_in, *data_out, ack ? "true" : "false");
  m_last_byte = data_in;
  return ack;
}

std::shared_ptr<MemoryCard> MemoryCard::Create()
{
  return std::make_shared<MemoryCard>();
}

u8 MemoryCard::ChecksumFrame(const u8* fptr)
{
  u8 value = fptr[0];
  for (u32 i = 0; i < SECTOR_SIZE; i++)
    value ^= fptr[i];

  return value;
}

void MemoryCard::Format()
{
  // fill everything with FF
  m_data.fill(u8(0xFF));

  // header
  {
    u8* fptr = GetSectorPtr(0);
    std::fill_n(fptr, SECTOR_SIZE, u8(0));
    fptr[0] = 'M';
    fptr[1] = 'C';
    fptr[0x7F] = ChecksumFrame(&m_data[0]);
  }

  // directory
  for (u32 frame = 1; frame < 16; frame++)
  {
    u8* fptr = GetSectorPtr(frame);
    std::fill_n(fptr, SECTOR_SIZE, u8(0));
    fptr[0] = 0xA0;                   // free
    fptr[0x7F] = ChecksumFrame(fptr); // checksum
  }

  // broken sector list
  for (u32 frame = 16; frame < 36; frame++)
  {
    u8* fptr = GetSectorPtr(frame);
    std::fill_n(fptr, SECTOR_SIZE, u8(0));
    fptr[0] = 0xFF;
    fptr[1] = 0xFF;
    fptr[2] = 0xFF;
    fptr[3] = 0xFF;
    fptr[0x7F] = ChecksumFrame(fptr); // checksum
  }

  // broken sector replacement data
  for (u32 frame = 36; frame < 56; frame++)
  {
    u8* fptr = GetSectorPtr(frame);
    std::fill_n(fptr, SECTOR_SIZE, u8(0xFF));
  }

  // unused frames
  for (u32 frame = 56; frame < 63; frame++)
  {
    u8* fptr = GetSectorPtr(frame);
    std::fill_n(fptr, SECTOR_SIZE, u8(0xFF));
  }

  // write test frame
  std::memcpy(GetSectorPtr(63), GetSectorPtr(0), SECTOR_SIZE);
}

u8* MemoryCard::GetSectorPtr(u32 sector)
{
  Assert(sector < NUM_SECTORS);
  return &m_data[sector * SECTOR_SIZE];
}
