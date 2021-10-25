#include "sio.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "controller.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "memory_card.h"
Log_SetChannel(SIO);

SIO g_sio;

SIO::SIO() = default;

SIO::~SIO() = default;

void SIO::Initialize()
{
  Reset();
}

void SIO::Shutdown() {}

void SIO::Reset()
{
  SoftReset();
}

bool SIO::DoState(StateWrapper& sw)
{
  sw.Do(&m_SIO_CTRL.bits);
  sw.Do(&m_SIO_STAT.bits);
  sw.Do(&m_SIO_MODE.bits);
  sw.Do(&m_SIO_BAUD);

  return !sw.HasError();
}

u32 SIO::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00: // SIO_DATA
    {
      Log_ErrorPrintf("Read SIO_DATA");

      const u8 value = 0xFF;
      return (ZeroExtend32(value) | (ZeroExtend32(value) << 8) | (ZeroExtend32(value) << 16) |
              (ZeroExtend32(value) << 24));
    }

    case 0x04: // SIO_STAT
    {
      const u32 bits = m_SIO_STAT.bits;
      return bits;
    }

    case 0x08: // SIO_MODE
      return ZeroExtend32(m_SIO_MODE.bits);

    case 0x0A: // SIO_CTRL
      return ZeroExtend32(m_SIO_CTRL.bits);

    case 0x0E: // SIO_BAUD
      return ZeroExtend32(m_SIO_BAUD);

    default:
      Log_ErrorPrintf("Unknown register read: 0x%X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void SIO::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: // SIO_DATA
    {
      Log_WarningPrintf("SIO_DATA (W) <- 0x%02X", value);
      return;
    }

    case 0x0A: // SIO_CTRL
    {
      Log_DebugPrintf("SIO_CTRL <- 0x%04X", value);

      m_SIO_CTRL.bits = Truncate16(value);
      if (m_SIO_CTRL.RESET)
        SoftReset();

      return;
    }

    case 0x08: // SIO_MODE
    {
      Log_DebugPrintf("SIO_MODE <- 0x%08X", value);
      m_SIO_MODE.bits = Truncate16(value);
      return;
    }

    case 0x0E:
    {
      Log_DebugPrintf("SIO_BAUD <- 0x%08X", value);
      m_SIO_BAUD = Truncate16(value);
      return;
    }

    default:
      Log_ErrorPrintf("Unknown register write: 0x%X <- 0x%08X", offset, value);
      return;
  }
}

void SIO::SoftReset()
{
  m_SIO_CTRL.bits = 0;
  m_SIO_STAT.bits = 0;
  m_SIO_STAT.DSRINPUTLEVEL = true;
  m_SIO_STAT.CTSINPUTLEVEL = true;
  m_SIO_STAT.TXDONE = true;
  m_SIO_STAT.TXRDY = true;
  m_SIO_MODE.bits = 0;
  m_SIO_BAUD = 0xDC;
}
