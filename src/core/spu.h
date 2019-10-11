#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>

class AudioStream;
class StateWrapper;

class System;
class DMA;
class InterruptController;

class SPU
{
public:
  using SampleFormat = s16;

  SPU();
  ~SPU();

  bool Initialize(System* system, DMA* dma, InterruptController* interrupt_controller);
  void Reset();
  bool DoState(StateWrapper& sw);

  u16 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u16 value);

  u32 DMARead();
  void DMAWrite(u32 value);

  void Execute(TickCount ticks);

  // Render statistics debug window.
  void DrawDebugWindow();

  // Manipulating debug options.
  void DrawDebugMenu();

private:
  static constexpr u32 RAM_SIZE = 512 * 1024;
  static constexpr u32 RAM_MASK = RAM_SIZE - 1;
  static constexpr u32 SPU_BASE = 0x1F801C00;
  static constexpr u32 NUM_VOICES = 24;
  static constexpr u32 NUM_VOICE_REGISTERS = 8;
  static constexpr u32 VOICE_ADDRESS_SHIFT = 3;
  static constexpr u32 NUM_SAMPLES_PER_ADPCM_BLOCK = 28;
  static constexpr u32 SAMPLE_RATE = 44100;
  static constexpr u32 SYSCLK_TICKS_PER_SPU_TICK = MASTER_CLOCK / SAMPLE_RATE; // 0x300

  enum class RAMTransferMode : u8
  {
    Stopped = 0,
    ManualWrite = 1,
    DMAWrite = 2,
    DMARead = 3
  };

  union SPUCNT
  {
    u16 bits;

    BitField<u16, bool, 15, 1> enable;
    BitField<u16, bool, 14, 1> mute;
    BitField<u16, u8, 10, 4> noise_frequency_shift;
    BitField<u16, u8, 8, 2> noise_frequency_step;
    BitField<u16, bool, 7, 1> reverb_master_enable;
    BitField<u16, bool, 6, 1> irq9_enable;
    BitField<u16, RAMTransferMode, 4, 2> ram_transfer_mode;
    BitField<u16, bool, 3, 1> external_audio_reverb;
    BitField<u16, bool, 2, 1> cd_audio_reverb;
    BitField<u16, bool, 1, 1> external_audio_enable;
    BitField<u16, bool, 0, 1> cd_audio_enable;

    BitField<u16, u8, 0, 6> mode;
  };

  union SPUSTAT
  {
    u16 bits;

    BitField<u16, bool, 11, 1> second_half_capture_buffer;
    BitField<u16, bool, 10, 1> transfer_busy;
    BitField<u16, bool, 9, 1> dma_read_request;
    BitField<u16, bool, 8, 1> dma_write_request;
    BitField<u16, bool, 7, 1> dma_read_write_request;
    BitField<u16, bool, 6, 1> irq9_flag;
    BitField<u16, u8, 0, 6> mode;
  };

  union ADSRRegister
  {
    u32 bits;
    struct
    {
      u16 bits_low;
      u16 bits_high;
    };

    BitField<u32, u8, 0, 4> sustain_level;
    BitField<u32, u8, 4, 4> decay_shift;
    BitField<u32, u8, 8, 2> attack_step;
    BitField<u32, u8, 10, 6> attack_shift;
    BitField<u32, bool, 15, 1> attack_exponential;

    BitField<u32, u8, 16, 5> release_shift;
    BitField<u32, bool, 21, 1> release_exponential;
    BitField<u32, u8, 22, 2> sustain_step;
    BitField<u32, u8, 24, 5> sustain_shift;
    BitField<u32, bool, 30, 1> sustain_direction_decrease;
    BitField<u32, bool, 31, 1> sustain_exponential;
  };

  union VolumeRegister
  {
    u16 bits;

    BitField<u16, bool, 15, 1> sweep_mode;
    BitField<u16, s16, 0, 15> fixed_volume; // divided by 2

    BitField<u16, bool, 14, 1> sweep_exponential;
    BitField<u16, bool, 13, 1> sweep_direction_decrease;
    BitField<u16, bool, 12, 1> sweep_phase_negative;
    BitField<u16, u8, 2, 5> sweep_shift;
    BitField<u16, u8, 0, 2> sweep_step;

    s16 GetVolume() { return fixed_volume * 2; }
  };

  // organized so we can replace this with a u16 array in the future
  union VoiceRegisters
  {
    u16 index[NUM_VOICE_REGISTERS];

    struct
    {
      VolumeRegister volume_left;
      VolumeRegister volume_right;

      u16 adpcm_sample_rate;   // VxPitch
      u16 adpcm_start_address; // multiply by 8

      ADSRRegister adsr;
      u16 adsr_volume;

      u16 adpcm_repeat_address; // multiply by 8
    };
  };

  union VoiceCounter
  {
    // promoted to u32 because of overflow
    u32 bits;

    BitField<u32, u8, 4, 8> interpolation_index;
    BitField<u32, u8, 12, 5> sample_index;
  };

  struct ADPCMBlock
  {
    union
    {
      u8 bits;

      BitField<u8, u8, 0, 4> shift;
      BitField<u8, u8, 4, 3> filter;
    } shift_filter;
    union
    {
      u8 bits;

      BitField<u8, bool, 0, 1> loop_end;
      BitField<u8, bool, 1, 1> loop_repeat;
      BitField<u8, bool, 2, 1> loop_start;
    } flags;

    u8 data[NUM_SAMPLES_PER_ADPCM_BLOCK / 2];

    // For both 4bit and 8bit ADPCM, reserved shift values 13..15 will act same as shift=9).
    u8 GetShift() const
    {
      const u8 shift = shift_filter.shift;
      return (shift > 12) ? 9 : shift;
    }

    u8 GetFilter() const { return std::min<u8>(shift_filter.filter, 4); }

    u8 GetNibble(u32 index) const { return (data[index / 2] >> ((index % 2) * 4)) & 0x0F; }
  };

  struct Voice
  {
    u16 current_address;
    VoiceRegisters regs;
    VoiceCounter counter;
    ADPCMBlock current_block; // TODO Drop this after decoding
    std::array<SampleFormat, NUM_SAMPLES_PER_ADPCM_BLOCK> current_block_samples;
    std::array<SampleFormat, 3> previous_block_last_samples;
    std::array<s32, 2> adpcm_state;

    bool has_samples;
    bool key_on;

    void KeyOn();
    void KeyOff();

    void DecodeBlock();
    SampleFormat SampleBlock(s32 index) const;
    s16 Interpolate() const;
  };

  u16 ReadVoiceRegister(u32 offset);
  void WriteVoiceRegister(u32 offset, u16 value);

  void UpdateDMARequest();
  u16 RAMTransferRead();
  void RAMTransferWrite(u16 value);

  void ReadADPCMBlock(u16 address, ADPCMBlock* block);
  static void DecodeADPCMBlock(const ADPCMBlock& block, SampleFormat* out_samples, s32* state);
  std::tuple<SampleFormat, SampleFormat> SampleVoice(u32 voice_index);
  void GenerateSample();

  System* m_system = nullptr;
  DMA* m_dma = nullptr;
  InterruptController* m_interrupt_controller = nullptr;
  AudioStream* m_audio_stream = nullptr;
  bool m_debug_window_open = true;

  SPUCNT m_SPUCNT = {};
  SPUSTAT m_SPUSTAT = {};

  u16 m_transfer_address_reg = 0;
  u32 m_transfer_address = 0;

  u16 m_irq_address = 0;

  VolumeRegister m_main_volume_left = {};
  VolumeRegister m_main_volume_right = {};

  u32 m_key_on_register = 0;
  u32 m_key_off_register = 0;
  u32 m_endx_register = 0;
  u32 m_reverb_on_register = 0;

  TickCount m_ticks_carry = 0;

  std::array<Voice, NUM_VOICES> m_voices{};
  std::array<u8, RAM_SIZE> m_ram{};
};