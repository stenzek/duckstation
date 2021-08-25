#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "system.h"
#include "types.h"
#include <array>
#include <memory>

// Enable to dump all voices of the SPU audio individually.
// #define SPU_DUMP_ALL_VOICES 1

class StateWrapper;

namespace Common {
class WAVWriter;
}

class TimingEvent;

class SPU
{
public:
  enum : u32
  {
    RAM_SIZE = 512 * 1024,
    RAM_MASK = RAM_SIZE - 1,
  };

  SPU();
  ~SPU();

  void Initialize();
  void CPUClockChanged();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  u16 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u16 value);

  void DMARead(u32* words, u32 word_count);
  void DMAWrite(const u32* words, u32 word_count);

  // Render statistics debug window.
  void DrawDebugStateWindow();

  // Executes the SPU, generating any pending samples.
  void GeneratePendingSamples();

  /// Returns true if currently dumping audio.
  ALWAYS_INLINE bool IsDumpingAudio() const { return static_cast<bool>(m_dump_writer); }

  /// Starts dumping audio to file.
  bool StartDumpingAudio(const char* filename);

  /// Stops dumping audio to file, if started.
  bool StopDumpingAudio();

  /// Access to SPU RAM.
  const std::array<u8, RAM_SIZE>& GetRAM() const { return m_ram; }
  std::array<u8, RAM_SIZE>& GetRAM() { return m_ram; }

  /// Change output stream - used for runahead.
  ALWAYS_INLINE void SetAudioStream(AudioStream* stream) { m_audio_stream = stream; }

private:
  static constexpr u32 SPU_BASE = 0x1F801C00;
  static constexpr u32 NUM_VOICES = 24;
  static constexpr u32 NUM_VOICE_REGISTERS = 8;
  static constexpr u32 VOICE_ADDRESS_SHIFT = 3;
  static constexpr u32 NUM_SAMPLES_PER_ADPCM_BLOCK = 28;
  static constexpr u32 NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK = 3;
  static constexpr u32 SAMPLE_RATE = 44100;
  static constexpr u32 SYSCLK_TICKS_PER_SPU_TICK = System::MASTER_CLOCK / SAMPLE_RATE; // 0x300
  static constexpr s16 ENVELOPE_MIN_VOLUME = 0;
  static constexpr s16 ENVELOPE_MAX_VOLUME = 0x7FFF;
  static constexpr u32 CAPTURE_BUFFER_SIZE_PER_CHANNEL = 0x400;
  static constexpr u32 MINIMUM_TICKS_BETWEEN_KEY_ON_OFF = 2;
  static constexpr u32 NUM_REVERB_REGS = 32;
  static constexpr u32 FIFO_SIZE_IN_HALFWORDS = 32;
  static constexpr TickCount TRANSFER_TICKS_PER_HALFWORD = 16;

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
    BitField<u16, bool, 14, 1> mute_n;
    BitField<u16, u8, 8, 6> noise_clock;
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
    BitField<u16, bool, 9, 1> dma_write_request;
    BitField<u16, bool, 8, 1> dma_read_request;
    BitField<u16, bool, 7, 1> dma_request;
    BitField<u16, bool, 6, 1> irq9_flag;
    BitField<u16, u8, 0, 6> mode;
  };

  union TransferControl
  {
    u16 bits;

    BitField<u8, u8, 1, 3> mode;
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
    BitField<u32, u8, 4, 4> decay_rate_shr2;
    BitField<u32, u8, 8, 7> attack_rate;
    BitField<u32, bool, 15, 1> attack_exponential;

    BitField<u32, u8, 16, 5> release_rate_shr2;
    BitField<u32, bool, 21, 1> release_exponential;
    BitField<u32, u8, 22, 7> sustain_rate;
    BitField<u32, bool, 30, 1> sustain_direction_decrease;
    BitField<u32, bool, 31, 1> sustain_exponential;
  };

  union VolumeRegister
  {
    u16 bits;

    BitField<u16, bool, 15, 1> sweep_mode;
    BitField<u16, s16, 0, 15> fixed_volume_shr1; // divided by 2

    BitField<u16, bool, 14, 1> sweep_exponential;
    BitField<u16, bool, 13, 1> sweep_direction_decrease;
    BitField<u16, bool, 12, 1> sweep_phase_negative;
    BitField<u16, u8, 0, 7> sweep_rate;
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
      s16 adsr_volume;

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

  union ADPCMFlags
  {
    u8 bits;

    BitField<u8, bool, 0, 1> loop_end;
    BitField<u8, bool, 1, 1> loop_repeat;
    BitField<u8, bool, 2, 1> loop_start;
  };

  struct ADPCMBlock
  {
    union
    {
      u8 bits;

      BitField<u8, u8, 0, 4> shift;
      BitField<u8, u8, 4, 3> filter;
    } shift_filter;
    ADPCMFlags flags;
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

  struct VolumeEnvelope
  {
    s32 counter;
    u8 rate;
    bool decreasing;
    bool exponential;

    void Reset(u8 rate_, bool decreasing_, bool exponential_);
    s16 Tick(s16 current_level);
  };

  struct VolumeSweep
  {
    VolumeEnvelope envelope;
    bool envelope_active;
    s16 current_level;

    void Reset(VolumeRegister reg);
    void Tick();
  };

  enum class ADSRPhase : u8
  {
    Off = 0,
    Attack = 1,
    Decay = 2,
    Sustain = 3,
    Release = 4
  };

  struct Voice
  {
    u16 current_address;
    VoiceRegisters regs;
    VoiceCounter counter;
    ADPCMFlags current_block_flags;
    bool is_first_block;
    std::array<s16, NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + NUM_SAMPLES_PER_ADPCM_BLOCK> current_block_samples;
    std::array<s16, 2> adpcm_last_samples;
    s32 last_volume;

    VolumeSweep left_volume;
    VolumeSweep right_volume;

    VolumeEnvelope adsr_envelope;
    ADSRPhase adsr_phase;
    s16 adsr_target;
    bool has_samples;
    bool ignore_loop_address;

    bool IsOn() const { return adsr_phase != ADSRPhase::Off; }

    void KeyOn();
    void KeyOff();
    void ForceOff();

    void DecodeBlock(const ADPCMBlock& block);
    s32 Interpolate() const;

    // Switches to the specified phase, filling in target.
    void UpdateADSREnvelope();

    // Updates the ADSR volume/phase.
    void TickADSR();
  };

  struct ReverbRegisters
  {
    s16 vLOUT;
    s16 vROUT;
    u16 mBASE;

    union
    {
      struct
      {
        u16 FB_SRC_A;
        u16 FB_SRC_B;
        s16 IIR_ALPHA;
        s16 ACC_COEF_A;
        s16 ACC_COEF_B;
        s16 ACC_COEF_C;
        s16 ACC_COEF_D;
        s16 IIR_COEF;
        s16 FB_ALPHA;
        s16 FB_X;
        u16 IIR_DEST_A[2];
        u16 ACC_SRC_A[2];
        u16 ACC_SRC_B[2];
        u16 IIR_SRC_A[2];
        u16 IIR_DEST_B[2];
        u16 ACC_SRC_C[2];
        u16 ACC_SRC_D[2];
        u16 IIR_SRC_B[2];
        u16 MIX_DEST_A[2];
        u16 MIX_DEST_B[2];
        s16 IN_COEF[2];
      };

      u16 rev[NUM_REVERB_REGS];
    };
  };

  static constexpr s32 Clamp16(s32 value) { return (value < -0x8000) ? -0x8000 : (value > 0x7FFF) ? 0x7FFF : value; }

  static constexpr s32 ApplyVolume(s32 sample, s16 volume) { return (sample * s32(volume)) >> 15; }

  static ADSRPhase GetNextADSRPhase(ADSRPhase phase);

  ALWAYS_INLINE bool IsVoiceReverbEnabled(u32 i) const
  {
    return ConvertToBoolUnchecked((m_reverb_on_register >> i) & u32(1));
  }
  ALWAYS_INLINE bool IsVoiceNoiseEnabled(u32 i) const
  {
    return ConvertToBoolUnchecked((m_noise_mode_register >> i) & u32(1));
  }
  ALWAYS_INLINE bool IsPitchModulationEnabled(u32 i) const
  {
    return ((i > 0) && ConvertToBoolUnchecked((m_pitch_modulation_enable_register >> i) & u32(1)));
  }
  ALWAYS_INLINE s16 GetVoiceNoiseLevel() const { return static_cast<s16>(static_cast<u16>(m_noise_level)); }

  u16 ReadVoiceRegister(u32 offset);
  void WriteVoiceRegister(u32 offset, u16 value);

  ALWAYS_INLINE bool IsRAMIRQTriggerable() const { return m_SPUCNT.irq9_enable && !m_SPUSTAT.irq9_flag; }
  ALWAYS_INLINE bool CheckRAMIRQ(u32 address) const { return ((ZeroExtend32(m_irq_address) * 8) == address); }
  void TriggerRAMIRQ();
  void CheckForLateRAMIRQs();

  void WriteToCaptureBuffer(u32 index, s16 value);
  void IncrementCaptureBufferPosition();

  void ReadADPCMBlock(u16 address, ADPCMBlock* block);
  std::tuple<s32, s32> SampleVoice(u32 voice_index);

  void UpdateNoise();

  u32 ReverbMemoryAddress(u32 address) const;
  s16 ReverbRead(u32 address, s32 offset = 0);
  void ReverbWrite(u32 address, s16 data);
  void ProcessReverb(s16 left_in, s16 right_in, s32* left_out, s32* right_out);

  void Execute(TickCount ticks);
  void UpdateEventInterval();

  void ExecuteFIFOWriteToRAM(TickCount& ticks);
  void ExecuteFIFOReadFromRAM(TickCount& ticks);
  void ExecuteTransfer(TickCount ticks);
  void ManualTransferWrite(u16 value);
  void UpdateTransferEvent();
  void UpdateDMARequest();

  std::unique_ptr<TimingEvent> m_tick_event;
  std::unique_ptr<TimingEvent> m_transfer_event;
  std::unique_ptr<Common::WAVWriter> m_dump_writer;
  AudioStream* m_audio_stream = nullptr;
  TickCount m_ticks_carry = 0;
  TickCount m_cpu_ticks_per_spu_tick = 0;
  TickCount m_cpu_tick_divider = 0;

  SPUCNT m_SPUCNT = {};
  SPUSTAT m_SPUSTAT = {};

  TransferControl m_transfer_control = {};
  u16 m_transfer_address_reg = 0;
  u32 m_transfer_address = 0;

  u16 m_irq_address = 0;
  u16 m_capture_buffer_position = 0;

  VolumeRegister m_main_volume_left_reg = {};
  VolumeRegister m_main_volume_right_reg = {};
  VolumeSweep m_main_volume_left = {};
  VolumeSweep m_main_volume_right = {};

  s16 m_cd_audio_volume_left = 0;
  s16 m_cd_audio_volume_right = 0;

  s16 m_external_volume_left = 0;
  s16 m_external_volume_right = 0;

  u32 m_key_on_register = 0;
  u32 m_key_off_register = 0;
  u32 m_endx_register = 0;
  u32 m_pitch_modulation_enable_register = 0;

  u32 m_noise_mode_register = 0;
  u32 m_noise_count = 0;
  u32 m_noise_level = 0;

  u32 m_reverb_on_register = 0;
  u32 m_reverb_base_address = 0;
  u32 m_reverb_current_address = 0;
  ReverbRegisters m_reverb_registers{};
  std::array<std::array<s16, 128>, 2> m_reverb_downsample_buffer;
  std::array<std::array<s16, 64>, 2> m_reverb_upsample_buffer;
  s32 m_reverb_resample_buffer_position = 0;

  std::array<Voice, NUM_VOICES> m_voices{};

  InlineFIFOQueue<u16, FIFO_SIZE_IN_HALFWORDS> m_transfer_fifo;

  std::array<u8, RAM_SIZE> m_ram{};

#ifdef SPU_DUMP_ALL_VOICES
  // +1 for reverb output
  std::array<std::unique_ptr<Common::WAVWriter>, NUM_VOICES + 1> m_voice_dump_writers;
#endif
};

extern SPU g_spu;