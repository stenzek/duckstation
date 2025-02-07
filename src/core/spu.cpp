// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "spu.h"
#include "cdrom.h"
#include "dma.h"
#include "host.h"
#include "imgui.h"
#include "interrupt_controller.h"
#include "system.h"
#include "timing_event.h"

#include "util/audio_stream.h"
#include "util/imgui_manager.h"
#include "util/media_capture.h"
#include "util/state_wrapper.h"
#include "util/wav_reader_writer.h"

#include "common/bitfield.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/fifo_queue.h"
#include "common/log.h"
#include "common/path.h"

#include "IconsEmoji.h"
#include "fmt/format.h"

#include <memory>

LOG_CHANNEL(SPU);

// Enable to dump all voices of the SPU audio individually.
// #define SPU_DUMP_ALL_VOICES 1

ALWAYS_INLINE static constexpr s32 Clamp16(s32 value)
{
  return (value < -0x8000) ? -0x8000 : (value > 0x7FFF) ? 0x7FFF : value;
}

ALWAYS_INLINE static constexpr s32 ApplyVolume(s32 sample, s16 volume)
{
  return (sample * s32(volume)) >> 15;
}

namespace SPU {
namespace {

enum : u32
{
  SPU_BASE = 0x1F801C00,
  NUM_VOICES = 24,
  NUM_VOICE_REGISTERS = 8,
  VOICE_ADDRESS_SHIFT = 3,
  NUM_SAMPLES_PER_ADPCM_BLOCK = 28,
  NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK = 3,
  SYSCLK_TICKS_PER_SPU_TICK = static_cast<u32>(System::MASTER_CLOCK) / static_cast<u32>(SAMPLE_RATE), // 0x300
  CAPTURE_BUFFER_SIZE_PER_CHANNEL = 0x400,
  MINIMUM_TICKS_BETWEEN_KEY_ON_OFF = 2,
  NUM_REVERB_REGS = 32,
  FIFO_SIZE_IN_HALFWORDS = 32
};
enum : TickCount
{
  TRANSFER_TICKS_PER_HALFWORD = 16
};

enum class RAMTransferMode : u8
{
  Stopped = 0,
  ManualWrite = 1,
  DMAWrite = 2,
  DMARead = 3
};

union SPUCNTRegister
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

union SPUSTATRegister
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
    BitField<u8, u8, 4, 4> filter;
  } shift_filter;
  ADPCMFlags flags;
  u8 data[NUM_SAMPLES_PER_ADPCM_BLOCK / 2];

  // For both 4bit and 8bit ADPCM, reserved shift values 13..15 will act same as shift=9).
  u8 GetShift() const
  {
    const u8 shift = shift_filter.shift;
    return (shift > 12) ? 9 : shift;
  }

  u8 GetFilter() const { return shift_filter.filter; }

  u8 GetNibble(u32 index) const { return (data[index / 2] >> ((index % 2) * 4)) & 0x0F; }
};

struct VolumeEnvelope
{
  static constexpr s32 MIN_VOLUME = -32768;
  static constexpr s32 MAX_VOLUME = 32767;

  u32 counter;
  u16 counter_increment;
  s16 step;
  u8 rate;
  bool decreasing;
  bool exponential;
  bool phase_invert;

  void Reset(u8 rate_, u8 rate_mask_, bool decreasing_, bool exponential_, bool phase_invert_);
  bool Tick(s16& current_level);
};

struct VolumeSweep
{
  VolumeEnvelope envelope;
  s16 current_level;
  bool envelope_active;

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
} // namespace

template<bool COMPATIBILITY>
static bool DoCompatibleState(StateWrapper& sw);

static ADSRPhase GetNextADSRPhase(ADSRPhase phase);

static bool IsVoiceReverbEnabled(u32 i);
static bool IsVoiceNoiseEnabled(u32 i);
static bool IsPitchModulationEnabled(u32 i);
static s16 GetVoiceNoiseLevel();

static u16 ReadVoiceRegister(u32 offset);
static void WriteVoiceRegister(u32 offset, u16 value);

static bool IsRAMIRQTriggerable();
static bool CheckRAMIRQ(u32 address);
static void TriggerRAMIRQ();
static void CheckForLateRAMIRQs();

static void WriteToCaptureBuffer(u32 index, s16 value);
static void IncrementCaptureBufferPosition();

static void ReadADPCMBlock(u16 address, ADPCMBlock* block);
static std::tuple<s32, s32> SampleVoice(u32 voice_index);

static void UpdateNoise();

static u32 ReverbMemoryAddress(u32 address);
static s16 ReverbRead(u32 address, s32 offset = 0);
static void ReverbWrite(u32 address, s16 data);
static void ProcessReverb(s32 left_in, s32 right_in, s32* left_out, s32* right_out);

static void InternalGeneratePendingSamples();
static void Execute(void* param, TickCount ticks, TickCount ticks_late);
static void UpdateEventInterval();

static void ExecuteFIFOWriteToRAM(TickCount& ticks);
static void ExecuteFIFOReadFromRAM(TickCount& ticks);
static void ExecuteTransfer(void* param, TickCount ticks, TickCount ticks_late);
static void ManualTransferWrite(u16 value);
static void UpdateTransferEvent();
static void UpdateDMARequest();

static void CreateOutputStream();

namespace {
struct SPUState
{
  TimingEvent transfer_event{"SPU Transfer", TRANSFER_TICKS_PER_HALFWORD, TRANSFER_TICKS_PER_HALFWORD,
                             &SPU::ExecuteTransfer, nullptr};
  TimingEvent tick_event{"SPU Sample", SYSCLK_TICKS_PER_SPU_TICK, SYSCLK_TICKS_PER_SPU_TICK, &SPU::Execute, nullptr};

  TickCount ticks_carry = 0;
  TickCount cpu_ticks_per_spu_tick = 0;
  TickCount cpu_tick_divider = 0;

  SPUCNTRegister SPUCNT = {};
  SPUSTATRegister SPUSTAT = {};

  TransferControl transfer_control = {};
  u16 transfer_address_reg = 0;
  u32 transfer_address = 0;

  u16 irq_address = 0;
  u16 capture_buffer_position = 0;

  VolumeRegister main_volume_left_reg = {};
  VolumeRegister main_volume_right_reg = {};
  VolumeSweep main_volume_left = {};
  VolumeSweep main_volume_right = {};

  s16 cd_audio_volume_left = 0;
  s16 cd_audio_volume_right = 0;

  s16 external_volume_left = 0;
  s16 external_volume_right = 0;

  u32 key_on_register = 0;
  u32 key_off_register = 0;
  u32 endx_register = 0;
  u32 pitch_modulation_enable_register = 0;

  u32 noise_mode_register = 0;
  u32 noise_count = 0;
  u32 noise_level = 0;

  u32 reverb_on_register = 0;
  u32 reverb_base_address = 0;
  u32 reverb_current_address = 0;
  ReverbRegisters reverb_registers{};
  std::array<std::array<s16, 128>, 2> reverb_downsample_buffer;
  std::array<std::array<s16, 64>, 2> reverb_upsample_buffer;
  s32 reverb_resample_buffer_position = 0;

  ALIGN_TO_CACHE_LINE std::array<Voice, NUM_VOICES> voices{};

  InlineFIFOQueue<u16, FIFO_SIZE_IN_HALFWORDS> transfer_fifo;

  std::unique_ptr<AudioStream> audio_stream;

  s16 last_reverb_input[2];
  s32 last_reverb_output[2];
  bool audio_output_muted = false;

#ifdef SPU_DUMP_ALL_VOICES
  // +1 for reverb output
  std::array<std::unique_ptr<WAVWriter>, NUM_VOICES + 1> s_voice_dump_writers;
#endif
};
} // namespace

ALIGN_TO_CACHE_LINE static SPUState s_state;
ALIGN_TO_CACHE_LINE static std::array<u8, RAM_SIZE> s_ram{};
ALIGN_TO_CACHE_LINE static std::array<s16, (44100 / 60) * 2> s_muted_output_buffer{};

} // namespace SPU

void SPU::Initialize()
{
  // (X * D) / N / 768 -> (X * D) / (N * 768)
  s_state.cpu_ticks_per_spu_tick = System::ScaleTicksToOverclock(SYSCLK_TICKS_PER_SPU_TICK);
  s_state.cpu_tick_divider = static_cast<TickCount>(g_settings.cpu_overclock_numerator * SYSCLK_TICKS_PER_SPU_TICK);
  s_state.tick_event.SetInterval(s_state.cpu_ticks_per_spu_tick);
  s_state.tick_event.SetPeriod(s_state.cpu_ticks_per_spu_tick);

  CreateOutputStream();
  Reset();

#ifdef SPU_DUMP_ALL_VOICES
  {
    const std::string base_path = System::GetNewMediaCapturePath(System::GetGameTitle(), "wav");
    for (size_t i = 0; i < s_state.s_voice_dump_writers.size(); i++)
    {
      s_state.s_voice_dump_writers[i].reset();
      s_state.s_voice_dump_writers[i] = std::make_unique<WAVWriter>();

      TinyString new_suffix;
      if (i == NUM_VOICES)
        new_suffix.assign("reverb.wav");
      else
        new_suffix.format("voice{}.wav", i);

      const std::string voice_filename = Path::ReplaceExtension(base_path, new_suffix);
      if (!s_state.s_voice_dump_writers[i]->Open(voice_filename.c_str(), SAMPLE_RATE, 2))
      {
        ERROR_LOG("Failed to open voice dump filename '{}'", voice_filename.c_str());
        s_state.s_voice_dump_writers[i].reset();
      }
    }
  }
#endif
}

void SPU::CreateOutputStream()
{
  INFO_LOG("Creating '{}' audio stream, sample rate = {}, buffer = {}, latency = {}{}, stretching = {}",
           AudioStream::GetBackendName(g_settings.audio_backend), static_cast<u32>(SAMPLE_RATE),
           g_settings.audio_stream_parameters.buffer_ms, g_settings.audio_stream_parameters.output_latency_ms,
           g_settings.audio_stream_parameters.output_latency_minimal ? " (or minimal)" : "",
           AudioStream::GetStretchModeName(g_settings.audio_stream_parameters.stretch_mode));

  Error error;
  s_state.audio_stream =
    AudioStream::CreateStream(g_settings.audio_backend, SAMPLE_RATE, g_settings.audio_stream_parameters,
                              g_settings.audio_driver.c_str(), g_settings.audio_output_device.c_str(), &error);
  if (!s_state.audio_stream)
  {
    Host::AddIconOSDWarning(
      "SPUAudioStream", ICON_EMOJI_WARNING,
      fmt::format(
        TRANSLATE_FS("SPU",
                     "Failed to create or configure audio stream, falling back to null output. The error was:\n{}"),
        error.GetDescription()),
      Host::OSD_ERROR_DURATION);
    s_state.audio_stream.reset();
    s_state.audio_stream = AudioStream::CreateNullStream(SAMPLE_RATE, g_settings.audio_stream_parameters.buffer_ms);
  }

  s_state.audio_stream->SetOutputVolume(System::GetAudioOutputVolume());
  s_state.audio_stream->SetNominalRate(System::GetAudioNominalRate());
  s_state.audio_stream->SetPaused(System::IsPaused());
}

void SPU::RecreateOutputStream()
{
  s_state.audio_stream.reset();
  CreateOutputStream();
}

void SPU::CPUClockChanged()
{
  // (X * D) / N / 768 -> (X * D) / (N * 768)
  s_state.cpu_ticks_per_spu_tick = System::ScaleTicksToOverclock(SYSCLK_TICKS_PER_SPU_TICK);
  s_state.cpu_tick_divider = static_cast<TickCount>(g_settings.cpu_overclock_numerator * SYSCLK_TICKS_PER_SPU_TICK);
  s_state.ticks_carry = 0;
  UpdateEventInterval();
}

void SPU::Shutdown()
{
#ifdef SPU_DUMP_ALL_VOICES
  for (size_t i = 0; i < s_state.s_voice_dump_writers.size(); i++)
    s_state.s_voice_dump_writers[i].reset();
#endif

  s_state.tick_event.Deactivate();
  s_state.transfer_event.Deactivate();
  s_state.audio_stream.reset();
}

void SPU::Reset()
{
  s_state.ticks_carry = 0;

  s_state.SPUCNT.bits = 0;
  s_state.SPUSTAT.bits = 0;
  s_state.transfer_address = 0;
  s_state.transfer_address_reg = 0;
  s_state.irq_address = 0;
  s_state.capture_buffer_position = 0;
  s_state.main_volume_left_reg.bits = 0;
  s_state.main_volume_right_reg.bits = 0;
  s_state.main_volume_left = {};
  s_state.main_volume_right = {};
  s_state.cd_audio_volume_left = 0;
  s_state.cd_audio_volume_right = 0;
  s_state.external_volume_left = 0;
  s_state.external_volume_right = 0;
  s_state.key_on_register = 0;
  s_state.key_off_register = 0;
  s_state.endx_register = 0;
  s_state.pitch_modulation_enable_register = 0;

  s_state.noise_mode_register = 0;
  s_state.noise_count = 0;
  s_state.noise_level = 1;

  s_state.reverb_on_register = 0;
  s_state.reverb_registers = {};
  s_state.reverb_registers.mBASE = 0;
  s_state.reverb_base_address = s_state.reverb_current_address = ZeroExtend32(s_state.reverb_registers.mBASE) << 2;
  s_state.reverb_downsample_buffer = {};
  s_state.reverb_upsample_buffer = {};
  s_state.reverb_resample_buffer_position = 0;

  for (u32 i = 0; i < NUM_VOICES; i++)
  {
    Voice& v = s_state.voices[i];
    v.current_address = 0;
    std::fill_n(v.regs.index, NUM_VOICE_REGISTERS, u16(0));
    v.counter.bits = 0;
    v.current_block_flags.bits = 0;
    v.is_first_block = 0;
    v.current_block_samples.fill(s16(0));
    v.adpcm_last_samples.fill(s32(0));
    v.adsr_envelope.Reset(0, 0, false, false, false);
    v.adsr_phase = ADSRPhase::Off;
    v.adsr_target = 0;
    v.has_samples = false;
    v.ignore_loop_address = false;
  }

  s_state.tick_event.Deactivate();
  s_state.transfer_event.Deactivate();
  s_state.transfer_fifo.Clear();
  s_ram.fill(0);
  UpdateEventInterval();
}

template<bool COMPATIBILITY>
bool SPU::DoCompatibleState(StateWrapper& sw)
{
  struct OldEnvelope
  {
    s32 counter;
    u8 rate;
    bool decreasing;
    bool exponential;
    bool phase_invert;
  };
  struct OldSweep
  {
    OldEnvelope env;
    bool envelope_active;
    s16 current_level;
  };

  static constexpr const auto do_compatible_volume_envelope = [](StateWrapper& sw, VolumeEnvelope* env) {
    if constexpr (COMPATIBILITY)
    {
      if (sw.GetVersion() < 70) [[unlikely]]
      {
        OldEnvelope oenv;
        sw.DoPOD(&oenv);
        env->Reset(oenv.rate, 0x7f, oenv.decreasing, oenv.exponential, oenv.phase_invert);
        env->counter = oenv.counter; // wrong
        return;
      }
    }

    sw.DoPOD(env);
  };
  static constexpr const auto do_compatible_volume_sweep = [](StateWrapper& sw, VolumeSweep* sweep) {
    if constexpr (COMPATIBILITY)
    {
      if (sw.GetVersion() < 70) [[unlikely]]
      {
        OldSweep osweep;
        sw.DoPOD(&osweep);
        sweep->envelope.Reset(osweep.env.rate, 0x7f, osweep.env.decreasing, osweep.env.exponential,
                              osweep.env.phase_invert);
        sweep->envelope.counter = osweep.env.counter; // wrong
        sweep->envelope_active = osweep.envelope_active;
        sweep->current_level = osweep.current_level;
        return;
      }
    }

    sw.DoPOD(sweep);
  };

  sw.Do(&s_state.ticks_carry);
  sw.Do(&s_state.SPUCNT.bits);
  sw.Do(&s_state.SPUSTAT.bits);
  sw.Do(&s_state.transfer_control.bits);
  sw.Do(&s_state.transfer_address);
  sw.Do(&s_state.transfer_address_reg);
  sw.Do(&s_state.irq_address);
  sw.Do(&s_state.capture_buffer_position);
  sw.Do(&s_state.main_volume_left_reg.bits);
  sw.Do(&s_state.main_volume_right_reg.bits);
  do_compatible_volume_sweep(sw, &s_state.main_volume_left);
  do_compatible_volume_sweep(sw, &s_state.main_volume_right);
  sw.Do(&s_state.cd_audio_volume_left);
  sw.Do(&s_state.cd_audio_volume_right);
  sw.Do(&s_state.external_volume_left);
  sw.Do(&s_state.external_volume_right);
  sw.Do(&s_state.key_on_register);
  sw.Do(&s_state.key_off_register);
  sw.Do(&s_state.endx_register);
  sw.Do(&s_state.pitch_modulation_enable_register);
  sw.Do(&s_state.noise_mode_register);
  sw.Do(&s_state.noise_count);
  sw.Do(&s_state.noise_level);
  sw.Do(&s_state.reverb_on_register);
  sw.Do(&s_state.reverb_base_address);
  sw.Do(&s_state.reverb_current_address);
  sw.Do(&s_state.reverb_registers.vLOUT);
  sw.Do(&s_state.reverb_registers.vROUT);
  sw.Do(&s_state.reverb_registers.mBASE);
  sw.DoArray(s_state.reverb_registers.rev, NUM_REVERB_REGS);
  for (u32 i = 0; i < 2; i++)
    sw.DoArray(s_state.reverb_downsample_buffer.data(), s_state.reverb_downsample_buffer.size());
  for (u32 i = 0; i < 2; i++)
    sw.DoArray(s_state.reverb_upsample_buffer.data(), s_state.reverb_upsample_buffer.size());
  sw.Do(&s_state.reverb_resample_buffer_position);
  for (u32 i = 0; i < NUM_VOICES; i++)
  {
    Voice& v = s_state.voices[i];
    sw.Do(&v.current_address);
    sw.DoArray(v.regs.index, NUM_VOICE_REGISTERS);
    sw.Do(&v.counter.bits);
    sw.Do(&v.current_block_flags.bits);
    if constexpr (COMPATIBILITY)
      sw.DoEx(&v.is_first_block, 47, false);
    else
      sw.Do(&v.is_first_block);
    sw.DoArray(&v.current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK], NUM_SAMPLES_PER_ADPCM_BLOCK);
    sw.DoArray(&v.current_block_samples[0], NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK);
    sw.Do(&v.adpcm_last_samples);
    sw.Do(&v.last_volume);
    do_compatible_volume_sweep(sw, &v.left_volume);
    do_compatible_volume_sweep(sw, &v.right_volume);
    do_compatible_volume_envelope(sw, &v.adsr_envelope);
    sw.Do(&v.adsr_phase);
    sw.Do(&v.adsr_target);
    sw.Do(&v.has_samples);
    sw.Do(&v.ignore_loop_address);
  }

  sw.Do(&s_state.transfer_fifo);
  sw.DoBytes(s_ram.data(), RAM_SIZE);

  if (sw.IsReading())
  {
    UpdateEventInterval();
    UpdateTransferEvent();
  }

  return !sw.HasError();
}

bool SPU::DoState(StateWrapper& sw)
{
  if (sw.GetVersion() < 70) [[unlikely]]
    return DoCompatibleState<true>(sw);
  else
    return DoCompatibleState<false>(sw);
}

u16 SPU::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x1F801D80 - SPU_BASE:
      return s_state.main_volume_left_reg.bits;

    case 0x1F801D82 - SPU_BASE:
      return s_state.main_volume_right_reg.bits;

    case 0x1F801D84 - SPU_BASE:
      return s_state.reverb_registers.vLOUT;

    case 0x1F801D86 - SPU_BASE:
      return s_state.reverb_registers.vROUT;

    case 0x1F801D88 - SPU_BASE:
      return Truncate16(s_state.key_on_register);

    case 0x1F801D8A - SPU_BASE:
      return Truncate16(s_state.key_on_register >> 16);

    case 0x1F801D8C - SPU_BASE:
      return Truncate16(s_state.key_off_register);

    case 0x1F801D8E - SPU_BASE:
      return Truncate16(s_state.key_off_register >> 16);

    case 0x1F801D90 - SPU_BASE:
      return Truncate16(s_state.pitch_modulation_enable_register);

    case 0x1F801D92 - SPU_BASE:
      return Truncate16(s_state.pitch_modulation_enable_register >> 16);

    case 0x1F801D94 - SPU_BASE:
      return Truncate16(s_state.noise_mode_register);

    case 0x1F801D96 - SPU_BASE:
      return Truncate16(s_state.noise_mode_register >> 16);

    case 0x1F801D98 - SPU_BASE:
      return Truncate16(s_state.reverb_on_register);

    case 0x1F801D9A - SPU_BASE:
      return Truncate16(s_state.reverb_on_register >> 16);

    case 0x1F801D9C - SPU_BASE:
      return Truncate16(s_state.endx_register);

    case 0x1F801D9E - SPU_BASE:
      return Truncate16(s_state.endx_register >> 16);

    case 0x1F801DA2 - SPU_BASE:
      return s_state.reverb_registers.mBASE;

    case 0x1F801DA4 - SPU_BASE:
      TRACE_LOG("SPU IRQ address -> 0x{:04X}", s_state.irq_address);
      return s_state.irq_address;

    case 0x1F801DA6 - SPU_BASE:
      TRACE_LOG("SPU transfer address register -> 0x{:04X}", s_state.transfer_address_reg);
      return s_state.transfer_address_reg;

    case 0x1F801DA8 - SPU_BASE:
      TRACE_LOG("SPU transfer data register read");
      return UINT16_C(0xFFFF);

    case 0x1F801DAA - SPU_BASE:
      TRACE_LOG("SPU control register -> 0x{:04X}", s_state.SPUCNT.bits);
      return s_state.SPUCNT.bits;

    case 0x1F801DAC - SPU_BASE:
      TRACE_LOG("SPU transfer control register -> 0x{:04X}", s_state.transfer_control.bits);
      return s_state.transfer_control.bits;

    case 0x1F801DAE - SPU_BASE:
      GeneratePendingSamples();
      TRACE_LOG("SPU status register -> 0x{:04X}", s_state.SPUCNT.bits);
      return s_state.SPUSTAT.bits;

    case 0x1F801DB0 - SPU_BASE:
      return s_state.cd_audio_volume_left;

    case 0x1F801DB2 - SPU_BASE:
      return s_state.cd_audio_volume_right;

    case 0x1F801DB4 - SPU_BASE:
      return s_state.external_volume_left;

    case 0x1F801DB6 - SPU_BASE:
      return s_state.external_volume_right;

    case 0x1F801DB8 - SPU_BASE:
      GeneratePendingSamples();
      return s_state.main_volume_left.current_level;

    case 0x1F801DBA - SPU_BASE:
      GeneratePendingSamples();
      return s_state.main_volume_right.current_level;

    default:
    {
      if (offset < (0x1F801D80 - SPU_BASE))
        return ReadVoiceRegister(offset);

      if (offset >= (0x1F801DC0 - SPU_BASE) && offset < (0x1F801E00 - SPU_BASE))
        return s_state.reverb_registers.rev[(offset - (0x1F801DC0 - SPU_BASE)) / 2];

      if (offset >= (0x1F801E00 - SPU_BASE) && offset < (0x1F801E60 - SPU_BASE))
      {
        const u32 voice_index = (offset - (0x1F801E00 - SPU_BASE)) / 4;
        GeneratePendingSamples();
        if (offset & 0x02)
          return s_state.voices[voice_index].left_volume.current_level;
        else
          return s_state.voices[voice_index].right_volume.current_level;
      }

      DEV_LOG("Unknown SPU register read: offset 0x{:X} (address 0x{:08X})", offset, offset | SPU_BASE);
      return UINT16_C(0xFFFF);
    }
  }
}

void SPU::WriteRegister(u32 offset, u16 value)
{
  switch (offset)
  {
    case 0x1F801D80 - SPU_BASE:
    {
      DEBUG_LOG("SPU main volume left <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.main_volume_left_reg.bits = value;
      s_state.main_volume_left.Reset(s_state.main_volume_left_reg);
      return;
    }

    case 0x1F801D82 - SPU_BASE:
    {
      DEBUG_LOG("SPU main volume right <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.main_volume_right_reg.bits = value;
      s_state.main_volume_right.Reset(s_state.main_volume_right_reg);
      return;
    }

    case 0x1F801D84 - SPU_BASE:
    {
      DEBUG_LOG("SPU reverb output volume left <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.reverb_registers.vLOUT = value;
      return;
    }

    case 0x1F801D86 - SPU_BASE:
    {
      DEBUG_LOG("SPU reverb output volume right <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.reverb_registers.vROUT = value;
      return;
    }

    case 0x1F801D88 - SPU_BASE:
    {
      DEBUG_LOG("SPU key on low <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.key_on_register = (s_state.key_on_register & 0xFFFF0000) | ZeroExtend32(value);
    }
    break;

    case 0x1F801D8A - SPU_BASE:
    {
      DEBUG_LOG("SPU key on high <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.key_on_register = (s_state.key_on_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);
    }
    break;

    case 0x1F801D8C - SPU_BASE:
    {
      DEBUG_LOG("SPU key off low <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.key_off_register = (s_state.key_off_register & 0xFFFF0000) | ZeroExtend32(value);
    }
    break;

    case 0x1F801D8E - SPU_BASE:
    {
      DEBUG_LOG("SPU key off high <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.key_off_register = (s_state.key_off_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);
    }
    break;

    case 0x1F801D90 - SPU_BASE:
    {
      GeneratePendingSamples();
      s_state.pitch_modulation_enable_register =
        (s_state.pitch_modulation_enable_register & 0xFFFF0000) | ZeroExtend32(value);
      DEBUG_LOG("SPU pitch modulation enable register <- 0x{:08X}", s_state.pitch_modulation_enable_register);
    }
    break;

    case 0x1F801D92 - SPU_BASE:
    {
      GeneratePendingSamples();
      s_state.pitch_modulation_enable_register =
        (s_state.pitch_modulation_enable_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);
      DEBUG_LOG("SPU pitch modulation enable register <- 0x{:08X}", s_state.pitch_modulation_enable_register);
    }
    break;

    case 0x1F801D94 - SPU_BASE:
    {
      DEBUG_LOG("SPU noise mode register <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.noise_mode_register = (s_state.noise_mode_register & 0xFFFF0000) | ZeroExtend32(value);
    }
    break;

    case 0x1F801D96 - SPU_BASE:
    {
      DEBUG_LOG("SPU noise mode register <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.noise_mode_register = (s_state.noise_mode_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);
    }
    break;

    case 0x1F801D98 - SPU_BASE:
    {
      DEBUG_LOG("SPU reverb on register <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.reverb_on_register = (s_state.reverb_on_register & 0xFFFF0000) | ZeroExtend32(value);
    }
    break;

    case 0x1F801D9A - SPU_BASE:
    {
      DEBUG_LOG("SPU reverb on register <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.reverb_on_register = (s_state.reverb_on_register & 0x0000FFFF) | (ZeroExtend32(value) << 16);
    }
    break;

    case 0x1F801DA2 - SPU_BASE:
    {
      DEBUG_LOG("SPU reverb base address < 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.reverb_registers.mBASE = value;
      s_state.reverb_base_address = ZeroExtend32(value << 2) & 0x3FFFFu;
      s_state.reverb_current_address = s_state.reverb_base_address;
    }
    break;

    case 0x1F801DA4 - SPU_BASE:
    {
      DEBUG_LOG("SPU IRQ address register <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.irq_address = value;

      if (IsRAMIRQTriggerable())
        CheckForLateRAMIRQs();

      return;
    }

    case 0x1F801DA6 - SPU_BASE:
    {
      DEBUG_LOG("SPU transfer address register <- 0x{:04X}", value);
      s_state.transfer_event.InvokeEarly();
      s_state.transfer_address_reg = value;
      s_state.transfer_address = ZeroExtend32(value) * 8;
      if (IsRAMIRQTriggerable() && CheckRAMIRQ(s_state.transfer_address))
      {
        DEBUG_LOG("Trigger IRQ @ {:08X} {:04X} from transfer address reg set", s_state.transfer_address,
                  s_state.transfer_address / 8);
        TriggerRAMIRQ();
      }
      return;
    }

    case 0x1F801DA8 - SPU_BASE:
    {
      TRACE_LOG("SPU transfer data register <- 0x{:04X} (RAM offset 0x{:08X})", ZeroExtend32(value),
                s_state.transfer_address);

      ManualTransferWrite(value);
      return;
    }

    case 0x1F801DAA - SPU_BASE:
    {
      DEBUG_LOG("SPU control register <- 0x{:04X}", value);
      GeneratePendingSamples();

      const SPUCNTRegister new_value{value};
      if (new_value.ram_transfer_mode != s_state.SPUCNT.ram_transfer_mode &&
          new_value.ram_transfer_mode == RAMTransferMode::Stopped)
      {
        // clear the fifo here?
        if (!s_state.transfer_fifo.IsEmpty())
        {
          if (s_state.SPUCNT.ram_transfer_mode == RAMTransferMode::DMAWrite)
          {
            // I would guess on the console it would gradually write the FIFO out. Hopefully nothing relies on this
            // level of timing granularity if we force it all out here.
            WARNING_LOG("Draining write SPU transfer FIFO with {} bytes left", s_state.transfer_fifo.GetSize());
            TickCount ticks = std::numeric_limits<TickCount>::max();
            ExecuteFIFOWriteToRAM(ticks);
            DebugAssert(s_state.transfer_fifo.IsEmpty());
          }
          else
          {
            DEBUG_LOG("Clearing read SPU transfer FIFO with {} bytes left", s_state.transfer_fifo.GetSize());
            s_state.transfer_fifo.Clear();
          }
        }
      }

      if (!new_value.enable && s_state.SPUCNT.enable)
      {
        // Mute all voices.
        // Interestingly, hardware tests found this seems to happen immediately, not on the next 44100hz cycle.
        for (u32 i = 0; i < NUM_VOICES; i++)
          s_state.voices[i].ForceOff();
      }

      s_state.SPUCNT.bits = new_value.bits;
      s_state.SPUSTAT.mode = s_state.SPUCNT.mode.GetValue();

      if (!s_state.SPUCNT.irq9_enable)
      {
        s_state.SPUSTAT.irq9_flag = false;
        InterruptController::SetLineState(InterruptController::IRQ::SPU, false);
      }
      else if (IsRAMIRQTriggerable())
      {
        CheckForLateRAMIRQs();
      }

      UpdateEventInterval();
      UpdateDMARequest();
      UpdateTransferEvent();
      return;
    }

    case 0x1F801DAC - SPU_BASE:
    {
      DEBUG_LOG("SPU transfer control register <- 0x{:04X}", value);
      s_state.transfer_control.bits = value;
      return;
    }

    case 0x1F801DB0 - SPU_BASE:
    {
      DEBUG_LOG("SPU left cd audio register <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.cd_audio_volume_left = value;
    }
    break;

    case 0x1F801DB2 - SPU_BASE:
    {
      DEBUG_LOG("SPU right cd audio register <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.cd_audio_volume_right = value;
    }
    break;

    case 0x1F801DB4 - SPU_BASE:
    {
      // External volumes aren't used, so don't bother syncing.
      DEBUG_LOG("SPU left external volume register <- 0x{:04X}", value);
      s_state.external_volume_left = value;
    }
    break;

    case 0x1F801DB6 - SPU_BASE:
    {
      // External volumes aren't used, so don't bother syncing.
      DEBUG_LOG("SPU right external volume register <- 0x{:04X}", value);
      s_state.external_volume_right = value;
    }
    break;

    case 0x1F801DB8 - SPU_BASE:
    {
      DEBUG_LOG("SPU main left volume register <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.main_volume_left.current_level = value;
    }
    break;

    case 0x1F801DBA - SPU_BASE:
    {
      DEBUG_LOG("SPU main right volume register <- 0x{:04X}", value);
      GeneratePendingSamples();
      s_state.main_volume_right.current_level = value;
    }
    break;

      // read-only registers
    case 0x1F801DAE - SPU_BASE:
    {
      return;
    }

    default:
    {
      if (offset < (0x1F801D80 - SPU_BASE))
      {
        WriteVoiceRegister(offset, value);
        return;
      }

      if (offset >= (0x1F801DC0 - SPU_BASE) && offset < (0x1F801E00 - SPU_BASE))
      {
        const u32 reg = (offset - (0x1F801DC0 - SPU_BASE)) / 2;
        DEBUG_LOG("SPU reverb register {} <- 0x{:04X}", reg, value);
        GeneratePendingSamples();
        s_state.reverb_registers.rev[reg] = value;
        return;
      }

      DEV_LOG("Unknown SPU register write: offset 0x{:X} (address 0x{:08X}) value 0x{:04X}", offset, offset | SPU_BASE,
              value);
      return;
    }
  }
}

u16 SPU::ReadVoiceRegister(u32 offset)
{
  const u32 reg_index = (offset % 0x10) / 2; //(offset & 0x0F) / 2;
  const u32 voice_index = (offset / 0x10);   //((offset >> 4) & 0x1F);
  Assert(voice_index < 24);

  // ADSR volume needs to be updated when reading. A voice might be off as well, but key on is pending.
  const Voice& voice = s_state.voices[voice_index];
  if (reg_index >= 6 && (voice.IsOn() || s_state.key_on_register & (1u << voice_index)))
    GeneratePendingSamples();

  TRACE_LOG("Read voice {} register {} -> 0x{:02X}", voice_index, reg_index, voice.regs.index[reg_index]);
  return voice.regs.index[reg_index];
}

void SPU::WriteVoiceRegister(u32 offset, u16 value)
{
  // per-voice registers
  const u32 reg_index = (offset % 0x10);
  const u32 voice_index = (offset / 0x10);
  DebugAssert(voice_index < 24);

  Voice& voice = s_state.voices[voice_index];
  if (voice.IsOn() || s_state.key_on_register & (1u << voice_index))
    GeneratePendingSamples();

  switch (reg_index)
  {
    case 0x00: // volume left
    {
      DEBUG_LOG("SPU voice {} volume left <- 0x{:04X}", voice_index, value);
      voice.regs.volume_left.bits = value;
      voice.left_volume.Reset(voice.regs.volume_left);
    }
    break;

    case 0x02: // volume right
    {
      DEBUG_LOG("SPU voice {} volume right <- 0x{:04X}", voice_index, value);
      voice.regs.volume_right.bits = value;
      voice.right_volume.Reset(voice.regs.volume_right);
    }
    break;

    case 0x04: // sample rate
    {
      DEBUG_LOG("SPU voice {} ADPCM sample rate <- 0x{:04X}", voice_index, value);
      voice.regs.adpcm_sample_rate = value;
    }
    break;

    case 0x06: // start address
    {
      DEBUG_LOG("SPU voice {} ADPCM start address <- 0x{:04X}", voice_index, value);
      voice.regs.adpcm_start_address = value;
    }
    break;

    case 0x08: // adsr low
    {
      DEBUG_LOG("SPU voice {} ADSR low <- 0x{:04X} (was 0x{:04X})", voice_index, value, voice.regs.adsr.bits_low);
      voice.regs.adsr.bits_low = value;
      if (voice.IsOn())
        voice.UpdateADSREnvelope();
    }
    break;

    case 0x0A: // adsr high
    {
      DEBUG_LOG("SPU voice {} ADSR high <- 0x{:04X} (was 0x{:04X})", voice_index, value, voice.regs.adsr.bits_low);
      voice.regs.adsr.bits_high = value;
      if (voice.IsOn())
        voice.UpdateADSREnvelope();
    }
    break;

    case 0x0C: // adsr volume
    {
      DEBUG_LOG("SPU voice {} ADSR volume <- 0x{:04X} (was 0x{:04X})", voice_index, value, voice.regs.adsr_volume);
      voice.regs.adsr_volume = value;
    }
    break;

    case 0x0E: // repeat address
    {
      // There is a short window of time here between the voice being keyed on and the first block finishing decoding
      // where setting the repeat address will *NOT* ignore the block/loop start flag.
      //
      // We always set this flag if the voice is off, because IRQs will keep the voice reading regardless, and we don't
      // want the address that we just set to get wiped out by the IRQ looping.
      //
      // Games sensitive to this are:
      //  - The Misadventures of Tron Bonne
      //  - Re-Loaded - The Hardcore Sequel (repeated sound effects)
      //  - Valkyrie Profile

      const bool ignore_loop_address = !voice.IsOn() || !voice.is_first_block;
      DEBUG_LOG("SPU voice {} ADPCM repeat address <- 0x{:04X}", voice_index, value);
      voice.regs.adpcm_repeat_address = value;
      voice.ignore_loop_address |= ignore_loop_address;

      if (!ignore_loop_address)
      {
        DEV_LOG("Not ignoring loop address, the ADPCM repeat address of 0x{:04X} for voice {} will be overwritten",
                value, voice_index);
      }
    }
    break;

    default:
    {
      ERROR_LOG("Unknown SPU voice {} register write: offset 0x%X (address 0x{:08X}) value 0x{:04X}", offset,
                voice_index, offset | SPU_BASE, ZeroExtend32(value));
    }
    break;
  }
}

bool SPU::IsVoiceReverbEnabled(u32 i)
{
  return ConvertToBoolUnchecked((s_state.reverb_on_register >> i) & u32(1));
}

bool SPU::IsVoiceNoiseEnabled(u32 i)
{
  return ConvertToBoolUnchecked((s_state.noise_mode_register >> i) & u32(1));
}

bool SPU::IsPitchModulationEnabled(u32 i)
{
  return ((i > 0) && ConvertToBoolUnchecked((s_state.pitch_modulation_enable_register >> i) & u32(1)));
}

s16 SPU::GetVoiceNoiseLevel()
{
  return static_cast<s16>(static_cast<u16>(s_state.noise_level));
}

bool SPU::IsRAMIRQTriggerable()
{
  return s_state.SPUCNT.irq9_enable && !s_state.SPUSTAT.irq9_flag;
}

bool SPU::CheckRAMIRQ(u32 address)
{
  return ((ZeroExtend32(s_state.irq_address) * 8) == address);
}

void SPU::TriggerRAMIRQ()
{
  DebugAssert(IsRAMIRQTriggerable());
  s_state.SPUSTAT.irq9_flag = true;
  InterruptController::SetLineState(InterruptController::IRQ::SPU, true);
}

void SPU::CheckForLateRAMIRQs()
{
  if (CheckRAMIRQ(s_state.transfer_address))
  {
    DEBUG_LOG("Trigger IRQ @ {:08X} {:04X} from late transfer", s_state.transfer_address, s_state.transfer_address / 8);
    TriggerRAMIRQ();
    return;
  }

  for (u32 i = 0; i < NUM_VOICES; i++)
  {
    // we skip voices which haven't started this block yet - because they'll check
    // the next time they're sampled, and the delay might be important.
    const Voice& v = s_state.voices[i];
    if (!v.has_samples)
      continue;

    const u32 address = v.current_address * 8;
    if (CheckRAMIRQ(address) || CheckRAMIRQ((address + 8) & RAM_MASK))
    {
      DEBUG_LOG("Trigger IRQ @ {:08X} ({:04X}) from late", address, address / 8);
      TriggerRAMIRQ();
      return;
    }
  }
}

void SPU::WriteToCaptureBuffer(u32 index, s16 value)
{
  const u32 ram_address = (index * CAPTURE_BUFFER_SIZE_PER_CHANNEL) | ZeroExtend16(s_state.capture_buffer_position);
  // Log_DebugFmt("write to capture buffer {} (0x{:08X}) <- 0x{:04X}", index, ram_address, u16(value));
  std::memcpy(&s_ram[ram_address], &value, sizeof(value));
  if (IsRAMIRQTriggerable() && CheckRAMIRQ(ram_address))
  {
    DEBUG_LOG("Trigger IRQ @ {:08X} ({:04X}) from capture buffer", ram_address, ram_address / 8);
    TriggerRAMIRQ();
  }
}

void SPU::IncrementCaptureBufferPosition()
{
  s_state.capture_buffer_position += sizeof(s16);
  s_state.capture_buffer_position %= CAPTURE_BUFFER_SIZE_PER_CHANNEL;
  s_state.SPUSTAT.second_half_capture_buffer = s_state.capture_buffer_position >= (CAPTURE_BUFFER_SIZE_PER_CHANNEL / 2);
}

ALWAYS_INLINE_RELEASE void SPU::ExecuteFIFOReadFromRAM(TickCount& ticks)
{
  while (ticks > 0 && !s_state.transfer_fifo.IsFull())
  {
    u16 value;
    std::memcpy(&value, &s_ram[s_state.transfer_address], sizeof(u16));
    s_state.transfer_address = (s_state.transfer_address + sizeof(u16)) & RAM_MASK;
    s_state.transfer_fifo.Push(value);
    ticks -= TRANSFER_TICKS_PER_HALFWORD;

    if (IsRAMIRQTriggerable() && CheckRAMIRQ(s_state.transfer_address))
    {
      DEBUG_LOG("Trigger IRQ @ {:08X} ({:04X}) from transfer read", s_state.transfer_address,
                s_state.transfer_address / 8);
      TriggerRAMIRQ();
    }
  }
}

ALWAYS_INLINE_RELEASE void SPU::ExecuteFIFOWriteToRAM(TickCount& ticks)
{
  while (ticks > 0 && !s_state.transfer_fifo.IsEmpty())
  {
    u16 value = s_state.transfer_fifo.Pop();
    std::memcpy(&s_ram[s_state.transfer_address], &value, sizeof(u16));
    s_state.transfer_address = (s_state.transfer_address + sizeof(u16)) & RAM_MASK;
    ticks -= TRANSFER_TICKS_PER_HALFWORD;

    if (IsRAMIRQTriggerable() && CheckRAMIRQ(s_state.transfer_address))
    {
      DEBUG_LOG("Trigger IRQ @ {:08X} ({:04X}) from transfer write", s_state.transfer_address,
                s_state.transfer_address / 8);
      TriggerRAMIRQ();
    }
  }
}

void SPU::ExecuteTransfer(void* param, TickCount ticks, TickCount ticks_late)
{
  const RAMTransferMode mode = s_state.SPUCNT.ram_transfer_mode;
  DebugAssert(mode != RAMTransferMode::Stopped);
  InternalGeneratePendingSamples();

  if (mode == RAMTransferMode::DMARead)
  {
    while (ticks > 0 && !s_state.transfer_fifo.IsFull())
    {
      ExecuteFIFOReadFromRAM(ticks);

      // this can result in the FIFO being emptied, hence double the while loop
      UpdateDMARequest();
    }

    // we're done if we have no more data to read
    if (s_state.transfer_fifo.IsFull())
    {
      s_state.SPUSTAT.transfer_busy = false;
      s_state.transfer_event.Deactivate();
      return;
    }

    s_state.SPUSTAT.transfer_busy = true;
    const TickCount ticks_until_complete =
      TickCount(s_state.transfer_fifo.GetSpace() * u32(TRANSFER_TICKS_PER_HALFWORD)) + ((ticks < 0) ? -ticks : 0);
    s_state.transfer_event.Schedule(ticks_until_complete);
  }
  else
  {
    // write the fifo to ram, request dma again when empty
    while (ticks > 0 && !s_state.transfer_fifo.IsEmpty())
    {
      ExecuteFIFOWriteToRAM(ticks);

      // similar deal here, the FIFO can be written out in a long slice
      UpdateDMARequest();
    }

    // we're done if we have no more data to write
    if (s_state.transfer_fifo.IsEmpty())
    {
      s_state.SPUSTAT.transfer_busy = false;
      s_state.transfer_event.Deactivate();
      return;
    }

    s_state.SPUSTAT.transfer_busy = true;
    const TickCount ticks_until_complete =
      TickCount(s_state.transfer_fifo.GetSize() * u32(TRANSFER_TICKS_PER_HALFWORD)) + ((ticks < 0) ? -ticks : 0);
    s_state.transfer_event.Schedule(ticks_until_complete);
  }
}

void SPU::ManualTransferWrite(u16 value)
{
  if (!s_state.transfer_fifo.IsEmpty() && s_state.SPUCNT.ram_transfer_mode != RAMTransferMode::DMARead)
  {
    WARNING_LOG("FIFO not empty on manual SPU write, draining to hopefully avoid corruption. Game is silly.");
    if (s_state.SPUCNT.ram_transfer_mode != RAMTransferMode::Stopped)
      ExecuteTransfer(nullptr, std::numeric_limits<s32>::max(), 0);
  }

  std::memcpy(&s_ram[s_state.transfer_address], &value, sizeof(u16));
  s_state.transfer_address = (s_state.transfer_address + sizeof(u16)) & RAM_MASK;

  if (IsRAMIRQTriggerable() && CheckRAMIRQ(s_state.transfer_address))
  {
    DEBUG_LOG("Trigger IRQ @ {:08X} ({:04X}) from manual write", s_state.transfer_address,
              s_state.transfer_address / 8);
    TriggerRAMIRQ();
  }
}

void SPU::UpdateTransferEvent()
{
  const RAMTransferMode mode = s_state.SPUCNT.ram_transfer_mode;
  if (mode == RAMTransferMode::Stopped)
  {
    s_state.transfer_event.Deactivate();
  }
  else if (mode == RAMTransferMode::DMARead)
  {
    // transfer event fills the fifo
    if (s_state.transfer_fifo.IsFull())
      s_state.transfer_event.Deactivate();
    else if (!s_state.transfer_event.IsActive())
      s_state.transfer_event.Schedule(TickCount(s_state.transfer_fifo.GetSpace() * u32(TRANSFER_TICKS_PER_HALFWORD)));
  }
  else
  {
    // transfer event copies from fifo to ram
    if (s_state.transfer_fifo.IsEmpty())
      s_state.transfer_event.Deactivate();
    else if (!s_state.transfer_event.IsActive())
      s_state.transfer_event.Schedule(TickCount(s_state.transfer_fifo.GetSize() * u32(TRANSFER_TICKS_PER_HALFWORD)));
  }

  s_state.SPUSTAT.transfer_busy = s_state.transfer_event.IsActive();
}

void SPU::UpdateDMARequest()
{
  switch (s_state.SPUCNT.ram_transfer_mode)
  {
    case RAMTransferMode::DMARead:
      s_state.SPUSTAT.dma_read_request = s_state.transfer_fifo.IsFull();
      s_state.SPUSTAT.dma_write_request = false;
      s_state.SPUSTAT.dma_request = s_state.SPUSTAT.dma_read_request;
      break;

    case RAMTransferMode::DMAWrite:
      s_state.SPUSTAT.dma_read_request = false;
      s_state.SPUSTAT.dma_write_request = s_state.transfer_fifo.IsEmpty();
      s_state.SPUSTAT.dma_request = s_state.SPUSTAT.dma_write_request;
      break;

    case RAMTransferMode::Stopped:
    case RAMTransferMode::ManualWrite:
    default:
      s_state.SPUSTAT.dma_read_request = false;
      s_state.SPUSTAT.dma_write_request = false;
      s_state.SPUSTAT.dma_request = false;
      break;
  }

  // This might call us back directly.
  DMA::SetRequest(DMA::Channel::SPU, s_state.SPUSTAT.dma_request);
}

void SPU::DMARead(u32* words, u32 word_count)
{
  /*
    From @JaCzekanski - behavior when block size is larger than the FIFO size
    for blocks <= 0x16 - all data is transferred correctly
    using block size 0x20 transfer behaves strange:
    % Writing 524288 bytes to SPU RAM to 0x00000000 using DMA... ok
    % Reading 256 bytes from SPU RAM from 0x00001000 using DMA... ok
    % 0x00001000: 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f ................
    % 0x00001010: 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f ................
    % 0x00001020: 20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f  !"#$%&'()*+,-./
    % 0x00001030: 30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f 0123456789:;<=>?
    % 0x00001040: 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f >?>?>?>?>?>?>?>?
    % 0x00001050: 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f >?>?>?>?>?>?>?>?
    % 0x00001060: 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f >?>?>?>?>?>?>?>?
    % 0x00001070: 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f 3e 3f >?>?>?>?>?>?>?>?
    % 0x00001080: 40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f @ABCDEFGHIJKLMNO
    % 0x00001090: 50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f PQRSTUVWXYZ[\]^_
    % 0x000010a0: 60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f `abcdefghijklmno
    % 0x000010b0: 70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f pqrstuvwxyz{|}~.
    % 0x000010c0: 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f ~.~.~.~.~.~.~.~.
    % 0x000010d0: 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f ~.~.~.~.~.~.~.~.
    % 0x000010e0: 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f ~.~.~.~.~.~.~.~.
    % 0x000010f0: 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f 7e 7f ~.~.~.~.~.~.~.~.
    Using Block size = 0x10 (correct data)
    % Reading 256 bytes from SPU RAM from 0x00001000 using DMA... ok
    % 0x00001000: 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f ................
    % 0x00001010: 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f ................
    % 0x00001020: 20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f  !"#$%&'()*+,-./
    % 0x00001030: 30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f 0123456789:;<=>?
    % 0x00001040: 40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f @ABCDEFGHIJKLMNO
    % 0x00001050: 50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f PQRSTUVWXYZ[\]^_
    % 0x00001060: 60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f `abcdefghijklmno
    % 0x00001070: 70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f pqrstuvwxyz{|}~.
    % 0x00001080: 80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f ................
    % 0x00001090: 90 91 92 93 94 95 96 97 98 99 9a 9b 9c 9d 9e 9f ................
    % 0x000010a0: a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af ................
    % 0x000010b0: b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bb bc bd be bf ................
    % 0x000010c0: c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd ce cf ................
    % 0x000010d0: d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de df ................
    % 0x000010e0: e0 e1 e2 e3 e4 e5 e6 e7 e8 e9 ea eb ec ed ee ef ................
    % 0x000010f0: f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 fa fb fc fd fe ff ................
   */

  u16* halfwords = reinterpret_cast<u16*>(words);
  u32 halfword_count = word_count * 2;

  const u32 size = s_state.transfer_fifo.GetSize();
  if (word_count > size)
  {
    u16 fill_value = 0;
    if (size > 0)
    {
      s_state.transfer_fifo.PopRange(halfwords, size);
      fill_value = halfwords[size - 1];
    }

    WARNING_LOG("Transfer FIFO underflow, filling with 0x{:04X}", fill_value);
    std::fill_n(&halfwords[size], halfword_count - size, fill_value);
  }
  else
  {
    s_state.transfer_fifo.PopRange(halfwords, halfword_count);
  }

  UpdateDMARequest();
  UpdateTransferEvent();
}

void SPU::DMAWrite(const u32* words, u32 word_count)
{
  const u16* halfwords = reinterpret_cast<const u16*>(words);
  u32 halfword_count = word_count * 2;

  const u32 words_to_transfer = std::min(s_state.transfer_fifo.GetSpace(), halfword_count);
  s_state.transfer_fifo.PushRange(halfwords, words_to_transfer);

  if (words_to_transfer != halfword_count) [[unlikely]]
    WARNING_LOG("Transfer FIFO overflow, dropping {} halfwords", halfword_count - words_to_transfer);

  UpdateDMARequest();
  UpdateTransferEvent();
}

void SPU::GeneratePendingSamples()
{
  if (s_state.transfer_event.IsActive())
    s_state.transfer_event.InvokeEarly();

  InternalGeneratePendingSamples();
}

void SPU::InternalGeneratePendingSamples()
{
  const TickCount ticks_pending = s_state.tick_event.GetTicksSinceLastExecution();
  TickCount frames_to_execute;
  if (g_settings.cpu_overclock_active)
  {
    frames_to_execute = static_cast<u32>((static_cast<u64>(ticks_pending) * g_settings.cpu_overclock_denominator) +
                                         static_cast<u32>(s_state.ticks_carry)) /
                        static_cast<u32>(s_state.cpu_tick_divider);
  }
  else
  {
    frames_to_execute = (ticks_pending + s_state.ticks_carry) / SYSCLK_TICKS_PER_SPU_TICK;
  }

  const bool force_exec = (frames_to_execute > 0);
  s_state.tick_event.InvokeEarly(force_exec);
}

const std::array<u8, SPU::RAM_SIZE>& SPU::GetRAM()
{
  return s_ram;
}

std::array<u8, SPU::RAM_SIZE>& SPU::GetWritableRAM()
{
  return s_ram;
}

bool SPU::IsAudioOutputMuted()
{
  return s_state.audio_output_muted;
}

void SPU::SetAudioOutputMuted(bool muted)
{
  s_state.audio_output_muted = muted;
}

AudioStream* SPU::GetOutputStream()
{
  return s_state.audio_stream.get();
}

void SPU::Voice::KeyOn()
{
  current_address = regs.adpcm_start_address & ~u16(1);
  counter.bits = 0;
  regs.adsr_volume = 0;
  adpcm_last_samples.fill(0);

  // Samples from the previous block for interpolation should be zero. Fixes clicks in audio in Breath of Fire III.
  std::fill_n(&current_block_samples[NUM_SAMPLES_PER_ADPCM_BLOCK], NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK,
              static_cast<s16>(0));

  has_samples = false;
  is_first_block = true;
  ignore_loop_address = false;
  adsr_phase = ADSRPhase::Attack;
  UpdateADSREnvelope();
}

void SPU::Voice::KeyOff()
{
  if (adsr_phase == ADSRPhase::Off || adsr_phase == ADSRPhase::Release)
    return;

  adsr_phase = ADSRPhase::Release;
  UpdateADSREnvelope();
}

void SPU::Voice::ForceOff()
{
  if (adsr_phase == ADSRPhase::Off)
    return;

  regs.adsr_volume = 0;
  adsr_phase = ADSRPhase::Off;
}

SPU::ADSRPhase SPU::GetNextADSRPhase(ADSRPhase phase)
{
  switch (phase)
  {
    case ADSRPhase::Attack:
      // attack -> decay
      return ADSRPhase::Decay;

    case ADSRPhase::Decay:
      // decay -> sustain
      return ADSRPhase::Sustain;

    case ADSRPhase::Sustain:
      // sustain stays in sustain until key off
      return ADSRPhase::Sustain;

    default:
    case ADSRPhase::Release:
      // end of release disables the voice
      return ADSRPhase::Off;
  }
}

void SPU::VolumeEnvelope::Reset(u8 rate_, u8 rate_mask_, bool decreasing_, bool exponential_, bool phase_invert_)
{
  rate = rate_;
  decreasing = decreasing_;
  exponential = exponential_;

  // psx-spx says "The Phase bit seems to have no effect in Exponential Decrease mode."
  // TODO: This needs to be tested on hardware.
  phase_invert = phase_invert_ && !(decreasing_ && exponential_);

  counter = 0;
  counter_increment = 0x8000;

  // negative level * negative step would give a positive number in decreasing+exponential mode, when we want it to be
  // negative. Phase invert cause the step to be positive in decreasing mode, otherwise negative. Bitwise NOT, so that
  // +7,+6,+5,+4 becomes -8,-7,-6,-5 as per psx-spx.
  const s16 base_step = 7 - (rate & 3);
  step = ((decreasing_ ^ phase_invert_) | (decreasing_ & exponential_)) ? ~base_step : base_step;
  if (rate < 44)
  {
    // AdsrStep = StepValue SHL Max(0,11-ShiftValue)
    step <<= (11 - (rate >> 2));
  }
  else if (rate >= 48)
  {
    // AdsrCycles = 1 SHL Max(0,ShiftValue-11)
    counter_increment >>= ((rate >> 2) - 11);

    // Rate of 0x7F (or more specifically all bits set, for decay/release) is a special case that never ticks.
    if ((rate & rate_mask_) != rate_mask_)
      counter_increment = std::max<u16>(counter_increment, 1u);
  }
}

ALWAYS_INLINE_RELEASE bool SPU::VolumeEnvelope::Tick(s16& current_level)
{
  // Recompute step in exponential/decrement mode.
  u32 this_increment = counter_increment;
  s32 this_step = step;
  if (exponential)
  {
    if (decreasing)
    {
      this_step = (this_step * current_level) >> 15;
    }
    else
    {
      if (current_level >= 0x6000)
      {
        if (rate < 40)
        {
          this_step >>= 2;
        }
        else if (rate >= 44)
        {
          this_increment >>= 2;
        }
        else
        {
          this_step >>= 1;
          this_increment >>= 1;
        }
      }
    }
  }

  counter += this_increment;

  // Very strange behavior. Rate of 0x76 behaves like 0x6A, seems it's dependent on the MSB=1.
  if (!(counter & 0x8000))
    return true;
  counter = 0;

  // Phase invert acts very strange. If the volume is positive, it will decrease to zero, then increase back to maximum
  // negative (inverted) volume. Except when decrementing, then it snaps straight to zero. Simply clamping to int16
  // range will be fine for incrementing, because the volume never decreases past zero. If the volume _was_ negative,
  // and is incrementing, hardware tests show that it only clamps to max, not 0.
  s32 new_level = current_level + this_step;
  if (!decreasing)
  {
    current_level = Truncate16(new_level = std::clamp(new_level, MIN_VOLUME, MAX_VOLUME));
    return (new_level != ((this_step < 0) ? MIN_VOLUME : MAX_VOLUME));
  }
  else
  {
    if (phase_invert)
      current_level = Truncate16(new_level = std::clamp(new_level, MIN_VOLUME, 0));
    else
      current_level = Truncate16(new_level = std::max(new_level, 0));
    return (new_level == 0);
  }
}

void SPU::VolumeSweep::Reset(VolumeRegister reg)
{
  if (!reg.sweep_mode)
  {
    current_level = reg.fixed_volume_shr1 * 2;
    envelope_active = false;
    return;
  }

  envelope.Reset(reg.sweep_rate, 0x7F, reg.sweep_direction_decrease, reg.sweep_exponential, reg.sweep_phase_negative);
  envelope_active = (envelope.counter_increment > 0);
}

void SPU::VolumeSweep::Tick()
{
  if (!envelope_active)
    return;

  envelope_active = envelope.Tick(current_level);
}

void SPU::Voice::UpdateADSREnvelope()
{
  switch (adsr_phase)
  {
    case ADSRPhase::Off:
      adsr_target = 0;
      adsr_envelope.Reset(0, 0, false, false, false);
      return;

    case ADSRPhase::Attack:
      adsr_target = 32767; // 0 -> max
      adsr_envelope.Reset(regs.adsr.attack_rate, 0x7F, false, regs.adsr.attack_exponential, false);
      break;

    case ADSRPhase::Decay:
      adsr_target = static_cast<s16>(std::min<s32>((u32(regs.adsr.sustain_level.GetValue()) + 1) * 0x800,
                                                   VolumeEnvelope::MAX_VOLUME)); // max -> sustain level
      adsr_envelope.Reset(regs.adsr.decay_rate_shr2 << 2, 0x1F << 2, true, true, false);
      break;

    case ADSRPhase::Sustain:
      adsr_target = 0;
      adsr_envelope.Reset(regs.adsr.sustain_rate, 0x7F, regs.adsr.sustain_direction_decrease,
                          regs.adsr.sustain_exponential, false);
      break;

    case ADSRPhase::Release:
      adsr_target = 0;
      adsr_envelope.Reset(regs.adsr.release_rate_shr2 << 2, 0x1F << 2, true, regs.adsr.release_exponential, false);
      break;

    default:
      break;
  }
}

void SPU::Voice::TickADSR()
{
  if (adsr_envelope.counter_increment > 0)
    adsr_envelope.Tick(regs.adsr_volume);

  if (adsr_phase != ADSRPhase::Sustain)
  {
    const bool reached_target =
      adsr_envelope.decreasing ? (regs.adsr_volume <= adsr_target) : (regs.adsr_volume >= adsr_target);
    if (reached_target)
    {
      adsr_phase = GetNextADSRPhase(adsr_phase);
      UpdateADSREnvelope();
    }
  }
}

void SPU::Voice::DecodeBlock(const ADPCMBlock& block)
{
  static constexpr std::array<s8, 16> filter_table_pos = {{0, 60, 115, 98, 122, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  static constexpr std::array<s8, 16> filter_table_neg = {{0, 0, -52, -55, -60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};

  // store samples needed for interpolation
  current_block_samples[2] = current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + NUM_SAMPLES_PER_ADPCM_BLOCK - 1];
  current_block_samples[1] = current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + NUM_SAMPLES_PER_ADPCM_BLOCK - 2];
  current_block_samples[0] = current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + NUM_SAMPLES_PER_ADPCM_BLOCK - 3];

  // pre-lookup
  const u8 shift = block.GetShift();
  const u8 filter_index = block.GetFilter();
  const s32 filter_pos = filter_table_pos[filter_index];
  const s32 filter_neg = filter_table_neg[filter_index];
  s16 last_samples[2] = {adpcm_last_samples[0], adpcm_last_samples[1]};

  // samples
  for (u32 i = 0; i < NUM_SAMPLES_PER_ADPCM_BLOCK; i++)
  {
    // extend 4-bit to 16-bit, apply shift from header and mix in previous samples
    s32 sample = s32(static_cast<s16>(ZeroExtend16(block.GetNibble(i)) << 12) >> shift);
    sample += (last_samples[0] * filter_pos) >> 6;
    sample += (last_samples[1] * filter_neg) >> 6;

    last_samples[1] = last_samples[0];
    current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + i] = last_samples[0] = static_cast<s16>(Clamp16(sample));
  }

  std::copy(last_samples, last_samples + countof(last_samples), adpcm_last_samples.begin());
  current_block_flags.bits = block.flags.bits;
}

s32 SPU::Voice::Interpolate() const
{
  static constexpr std::array<s16, 0x200> gauss = {{
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, //
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, //
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, //
    0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003, //
    0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007, //
    0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, //
    0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018, // entry
    0x0019, 0x001B, 0x001C, 0x001E, 0x0020, 0x0021, 0x0023, 0x0025, // 000..07F
    0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038, //
    0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050, //
    0x0054, 0x0057, 0x005B, 0x005F, 0x0063, 0x0067, 0x006B, 0x006F, //
    0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096, //
    0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7, //
    0x00CD, 0x00D4, 0x00DB, 0x00E3, 0x00EA, 0x00F2, 0x00FA, 0x0101, //
    0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148, //
    0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C, //
    0x01A8, 0x01B4, 0x01C0, 0x01CC, 0x01D9, 0x01E5, 0x01F2, 0x0200, //
    0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273, //
    0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9, //
    0x030B, 0x031D, 0x0330, 0x0343, 0x0356, 0x036A, 0x037E, 0x0392, //
    0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441, //
    0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506, //
    0x0520, 0x053B, 0x0556, 0x0572, 0x058E, 0x05AA, 0x05C7, 0x05E4, // entry
    0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC, // 080..0FF
    0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF, //
    0x0813, 0x0838, 0x085D, 0x0883, 0x08A9, 0x08D0, 0x08F7, 0x091E, //
    0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C, //
    0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8, //
    0x0C07, 0x0C38, 0x0C68, 0x0C99, 0x0CCB, 0x0CFD, 0x0D30, 0x0D63, //
    0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F, //
    0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB, //
    0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7, //
    0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4, //
    0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700, //
    0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B, //
    0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3, //
    0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37, //
    0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4, //
    0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389, // entry
    0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653, // 100..17F
    0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E, //
    0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18, //
    0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D, //
    0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209, //
    0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509, //
    0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807, //
    0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00, //
    0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF, //
    0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0, //
    0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C, //
    0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651, //
    0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9, //
    0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F, //
    0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0, //
    0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7, // entry
    0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0, // 180..1FF
    0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397, //
    0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529, //
    0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684, //
    0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3, //
    0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886, //
    0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A, //
    0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F, //
    0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3  //
  }};

  const u8 i = counter.interpolation_index;
  const u32 s = NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + ZeroExtend32(counter.sample_index.GetValue());

  s32 out = s32(gauss[0x0FF - i]) * s32(current_block_samples[s - 3]);
  out += s32(gauss[0x1FF - i]) * s32(current_block_samples[s - 2]);
  out += s32(gauss[0x100 + i]) * s32(current_block_samples[s - 1]);
  out += s32(gauss[0x000 + i]) * s32(current_block_samples[s - 0]);
  return out >> 15;
}

void SPU::ReadADPCMBlock(u16 address, ADPCMBlock* block)
{
  u32 ram_address = (ZeroExtend32(address) * 8) & RAM_MASK;
  if (IsRAMIRQTriggerable() && (CheckRAMIRQ(ram_address) || CheckRAMIRQ((ram_address + 8) & RAM_MASK)))
  {
    DEBUG_LOG("Trigger IRQ @ {:08X} ({:04X}) from ADPCM reader", ram_address, ram_address / 8);
    TriggerRAMIRQ();
  }

  // fast path - no wrap-around
  if ((ram_address + sizeof(ADPCMBlock)) <= RAM_SIZE)
  {
    std::memcpy(block, &s_ram[ram_address], sizeof(ADPCMBlock));
    return;
  }

  block->shift_filter.bits = s_ram[ram_address];
  ram_address = (ram_address + 1) & RAM_MASK;
  block->flags.bits = s_ram[ram_address];
  ram_address = (ram_address + 1) & RAM_MASK;
  for (u32 i = 0; i < 14; i++)
  {
    block->data[i] = s_ram[ram_address];
    ram_address = (ram_address + 1) & RAM_MASK;
  }
}

ALWAYS_INLINE_RELEASE std::tuple<s32, s32> SPU::SampleVoice(u32 voice_index)
{
  Voice& voice = s_state.voices[voice_index];
  if (!voice.IsOn() && !s_state.SPUCNT.irq9_enable)
  {
    voice.last_volume = 0;

#ifdef SPU_DUMP_ALL_VOICES
    if (s_state.s_voice_dump_writers[voice_index])
    {
      const s16 dump_samples[2] = {0, 0};
      s_state.s_voice_dump_writers[voice_index]->WriteFrames(dump_samples, 1);
    }
#endif

    return {};
  }

  if (!voice.has_samples)
  {
    ADPCMBlock block;
    ReadADPCMBlock(voice.current_address, &block);
    voice.DecodeBlock(block);
    voice.has_samples = true;

    if (voice.current_block_flags.loop_start && !voice.ignore_loop_address)
    {
      TRACE_LOG("Voice {} loop start @ 0x{:08X}", voice_index, voice.current_address);
      voice.regs.adpcm_repeat_address = voice.current_address;
    }
  }

  // skip interpolation when the volume is muted anyway
  s32 volume;
  if (voice.regs.adsr_volume != 0)
  {
    // interpolate/sample and apply ADSR volume
    s32 sample;
    if (IsVoiceNoiseEnabled(voice_index))
      sample = GetVoiceNoiseLevel();
    else
      sample = voice.Interpolate();

    volume = ApplyVolume(sample, voice.regs.adsr_volume);
  }
  else
  {
    volume = 0;
  }

  voice.last_volume = volume;

  if (voice.adsr_phase != ADSRPhase::Off)
    voice.TickADSR();

  // Pitch modulation
  u16 step = voice.regs.adpcm_sample_rate;
  if (IsPitchModulationEnabled(voice_index))
  {
    const s32 factor = std::clamp<s32>(s_state.voices[voice_index - 1].last_volume, -0x8000, 0x7FFF) + 0x8000;
    step = Truncate16(static_cast<u32>((SignExtend32(step) * factor) >> 15));
  }
  step = std::min<u16>(step, 0x3FFF);

  // Shouldn't ever overflow because if sample_index == 27, step == 0x4000 there won't be a carry out from the
  // interpolation index. If there is a carry out, bit 12 will never be 1, so it'll never add more than 4 to
  // sample_index, which should never be >27.
  DebugAssert(voice.counter.sample_index < NUM_SAMPLES_PER_ADPCM_BLOCK);
  voice.counter.bits += step;

  if (voice.counter.sample_index >= NUM_SAMPLES_PER_ADPCM_BLOCK)
  {
    // next block
    voice.counter.sample_index -= NUM_SAMPLES_PER_ADPCM_BLOCK;
    voice.has_samples = false;
    voice.is_first_block = false;
    voice.current_address += 2;

    // handle flags
    if (voice.current_block_flags.loop_end)
    {
      s_state.endx_register |= (u32(1) << voice_index);
      voice.current_address = voice.regs.adpcm_repeat_address & ~u16(1);

      if (!voice.current_block_flags.loop_repeat)
      {
        // End+Mute flags are ignored when noise is enabled. ADPCM data is still decoded.
        if (!IsVoiceNoiseEnabled(voice_index))
        {
          TRACE_LOG("Voice {} loop end+mute @ 0x{:04X}", voice_index, voice.current_address);
          voice.ForceOff();
        }
        else
        {
          TRACE_LOG("IGNORING voice {} loop end+mute @ 0x{:04X}", voice_index, voice.current_address);
        }
      }
      else
      {
        TRACE_LOG("Voice {} loop end+repeat @ 0x{:04X}", voice_index, voice.current_address);
      }
    }
  }

  // apply per-channel volume
  const s32 left = ApplyVolume(volume, voice.left_volume.current_level);
  const s32 right = ApplyVolume(volume, voice.right_volume.current_level);
  voice.left_volume.Tick();
  voice.right_volume.Tick();

#ifdef SPU_DUMP_ALL_VOICES
  if (s_state.s_voice_dump_writers[voice_index])
  {
    const s16 dump_samples[2] = {static_cast<s16>(Clamp16(left)), static_cast<s16>(Clamp16(right))};
    s_state.s_voice_dump_writers[voice_index]->WriteFrames(dump_samples, 1);
  }
#endif

  return std::make_tuple(left, right);
}

void SPU::UpdateNoise()
{
  // Dr Hell's noise waveform, implementation borrowed from pcsx-r.
  static constexpr std::array<u8, 64> noise_wave_add = {
    {1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
     0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1}};
  static constexpr std::array<u8, 5> noise_freq_add = {{0, 84, 140, 180, 210}};

  const u32 noise_clock = s_state.SPUCNT.noise_clock;
  const u32 level = (0x8000u >> (noise_clock >> 2)) << 16;

  s_state.noise_count += 0x10000u + noise_freq_add[noise_clock & 3u];
  if ((s_state.noise_count & 0xFFFFu) >= noise_freq_add[4])
  {
    s_state.noise_count += 0x10000;
    s_state.noise_count -= noise_freq_add[noise_clock & 3u];
  }

  if (s_state.noise_count < level)
    return;

  s_state.noise_count %= level;
  s_state.noise_level = (s_state.noise_level << 1) | noise_wave_add[(s_state.noise_level >> 10) & 63u];
}

u32 SPU::ReverbMemoryAddress(u32 address)
{
  // Ensures address does not leave the reverb work area.
  static constexpr u32 MASK = (RAM_SIZE - 1) / 2;
  u32 offset = s_state.reverb_current_address + (address & MASK);
  offset += s_state.reverb_base_address & ((s32)(offset << 13) >> 31);

  // We address RAM in bytes. TODO: Change this to words.
  return (offset & MASK) * 2u;
}

s16 SPU::ReverbRead(u32 address, s32 offset)
{
  // TODO: This should check interrupts.
  const u32 real_address = ReverbMemoryAddress((address << 2) + offset);

  s16 data;
  std::memcpy(&data, &s_ram[real_address], sizeof(data));
  return data;
}

void SPU::ReverbWrite(u32 address, s16 data)
{
  // TODO: This should check interrupts.
  const u32 real_address = ReverbMemoryAddress(address << 2);
  std::memcpy(&s_ram[real_address], &data, sizeof(data));
}

void SPU::ProcessReverb(s32 left_in, s32 right_in, s32* left_out, s32* right_out)
{
  // From PSX-SPX:
  // Input and output to/from the reverb unit is resampled using a 39-tap FIR filter with the following coefficients.
  //  -0001h,  0000h,  0002h,  0000h, -000Ah,  0000h,  0023h,  0000h,
  //  -0067h,  0000h,  010Ah,  0000h, -0268h,  0000h,  0534h,  0000h,
  //  -0B90h,  0000h,  2806h,  4000h,  2806h,  0000h, -0B90h,  0000h,
  //   0534h,  0000h, -0268h,  0000h,  010Ah,  0000h, -0067h,  0000h,
  //   0023h,  0000h, -000Ah,  0000h,  0002h,  0000h, -0001h
  //
  // Zeros have been removed since the result is always zero, therefore the multiply is redundant.

  alignas(VECTOR_ALIGNMENT) static constexpr std::array<s32, 20> resample_coeff = {
    -0x0001, 0x0002,  -0x000A, 0x0023,  -0x0067, 0x010A,  -0x0268, 0x0534,  -0x0B90, 0x2806,
    0x2806,  -0x0B90, 0x0534,  -0x0268, 0x010A,  -0x0067, 0x0023,  -0x000A, 0x0002,  -0x0001};

  static constexpr auto iiasm = [](const s16 insamp) {
    if (s_state.reverb_registers.IIR_ALPHA == -32768) [[unlikely]]
      return (insamp == -32768) ? 0 : (insamp * -65536);
    else
      return insamp * (32768 - s_state.reverb_registers.IIR_ALPHA);
  };

  static constexpr auto neg = [](s32 samp) { return (samp == -32768) ? 0x7FFF : -samp; };

  s_state.last_reverb_input[0] = Truncate16(left_in);
  s_state.last_reverb_input[1] = Truncate16(right_in);

  // Resampling buffer is duplicated to avoid having to manually wrap the index.
  s_state.reverb_downsample_buffer[0][s_state.reverb_resample_buffer_position | 0x00] =
    s_state.reverb_downsample_buffer[0][s_state.reverb_resample_buffer_position | 0x40] = Truncate16(left_in);
  s_state.reverb_downsample_buffer[1][s_state.reverb_resample_buffer_position | 0x00] =
    s_state.reverb_downsample_buffer[1][s_state.reverb_resample_buffer_position | 0x40] = Truncate16(right_in);

  // Reverb algorithm from Mednafen-PSX, rewritten/vectorized.
  s32 out[2];
  if (s_state.reverb_resample_buffer_position & 1u)
  {
    std::array<s32, 2> downsampled;
    for (size_t channel = 0; channel < 2; channel++)
    {
      const s16* src =
        &s_state.reverb_downsample_buffer[channel][(s_state.reverb_resample_buffer_position - 38) & 0x3F];
      GSVector4i acc =
        GSVector4i::load<true>(&resample_coeff[0]).mul32l(GSVector4i::load<false>(&src[0]).sll32(16).sra32(16));
      acc = acc.add32(
        GSVector4i::load<true>(&resample_coeff[4]).mul32l(GSVector4i::load<false>(&src[8]).sll32(16).sra32(16)));
      acc = acc.add32(
        GSVector4i::load<true>(&resample_coeff[8]).mul32l(GSVector4i::load<false>(&src[16]).sll32(16).sra32(16)));
      acc = acc.add32(
        GSVector4i::load<true>(&resample_coeff[12]).mul32l(GSVector4i::load<false>(&src[24]).sll32(16).sra32(16)));
      acc = acc.add32(
        GSVector4i::load<true>(&resample_coeff[16]).mul32l(GSVector4i::load<false>(&src[32]).sll32(16).sra32(16)));

      // Horizontal reduction, middle 0x4000. Moved here so we don't need another 4 elements above.
      downsampled[channel] = Clamp16((acc.addv_s32() + (0x4000 * src[19])) >> 15);
    }

    for (size_t channel = 0; channel < 2; channel++)
    {
      if (s_state.SPUCNT.reverb_master_enable)
      {
        // Input from Mixer (Input volume multiplied with incoming data).
        const s32 IIR_INPUT_A = Clamp16(
          (((ReverbRead(s_state.reverb_registers.IIR_SRC_A[channel ^ 0]) * s_state.reverb_registers.IIR_COEF) >> 14) +
           ((downsampled[channel] * s_state.reverb_registers.IN_COEF[channel]) >> 14)) >>
          1);
        const s32 IIR_INPUT_B = Clamp16(
          (((ReverbRead(s_state.reverb_registers.IIR_SRC_B[channel ^ 1]) * s_state.reverb_registers.IIR_COEF) >> 14) +
           ((downsampled[channel] * s_state.reverb_registers.IN_COEF[channel]) >> 14)) >>
          1);

        // Same Side Reflection (left-to-left and right-to-right).
        const s32 IIR_A = Clamp16((((IIR_INPUT_A * s_state.reverb_registers.IIR_ALPHA) >> 14) +
                                   (iiasm(ReverbRead(s_state.reverb_registers.IIR_DEST_A[channel], -1)) >> 14)) >>
                                  1);

        // Different Side Reflection (left-to-right and right-to-left).
        const s32 IIR_B = Clamp16((((IIR_INPUT_B * s_state.reverb_registers.IIR_ALPHA) >> 14) +
                                   (iiasm(ReverbRead(s_state.reverb_registers.IIR_DEST_B[channel], -1)) >> 14)) >>
                                  1);

        ReverbWrite(s_state.reverb_registers.IIR_DEST_A[channel], Truncate16(IIR_A));
        ReverbWrite(s_state.reverb_registers.IIR_DEST_B[channel], Truncate16(IIR_B));
      }

      // Early Echo (Comb Filter, with input from buffer).
      const s32 ACC =
        ((ReverbRead(s_state.reverb_registers.ACC_SRC_A[channel]) * s_state.reverb_registers.ACC_COEF_A) >> 14) +
        ((ReverbRead(s_state.reverb_registers.ACC_SRC_B[channel]) * s_state.reverb_registers.ACC_COEF_B) >> 14) +
        ((ReverbRead(s_state.reverb_registers.ACC_SRC_C[channel]) * s_state.reverb_registers.ACC_COEF_C) >> 14) +
        ((ReverbRead(s_state.reverb_registers.ACC_SRC_D[channel]) * s_state.reverb_registers.ACC_COEF_D) >> 14);

      // Late Reverb APF1 (All Pass Filter 1, with input from COMB).
      const s32 FB_A = ReverbRead(s_state.reverb_registers.MIX_DEST_A[channel] - s_state.reverb_registers.FB_SRC_A);
      const s32 FB_B = ReverbRead(s_state.reverb_registers.MIX_DEST_B[channel] - s_state.reverb_registers.FB_SRC_B);
      const s32 MDA = Clamp16((ACC + ((FB_A * neg(s_state.reverb_registers.FB_ALPHA)) >> 14)) >> 1);

      // Late Reverb APF2 (All Pass Filter 2, with input from APF1).
      const s32 MDB = Clamp16(FB_A + ((((MDA * s_state.reverb_registers.FB_ALPHA) >> 14) +
                                       ((FB_B * neg(s_state.reverb_registers.FB_X)) >> 14)) >>
                                      1));

      // 22050hz sample output.
      s_state.reverb_upsample_buffer[channel][(s_state.reverb_resample_buffer_position >> 1) | 0x20] =
        s_state.reverb_upsample_buffer[channel][s_state.reverb_resample_buffer_position >> 1] =
          Truncate16(Clamp16(FB_B + ((MDB * s_state.reverb_registers.FB_X) >> 15)));

      if (s_state.SPUCNT.reverb_master_enable)
      {
        ReverbWrite(s_state.reverb_registers.MIX_DEST_A[channel], Truncate16(MDA));
        ReverbWrite(s_state.reverb_registers.MIX_DEST_B[channel], Truncate16(MDB));
      }
    }

    s_state.reverb_current_address = (s_state.reverb_current_address + 1) & 0x3FFFFu;
    s_state.reverb_current_address =
      (s_state.reverb_current_address == 0) ? s_state.reverb_base_address : s_state.reverb_current_address;

    for (size_t channel = 0; channel < 2; channel++)
    {
      const s16* src =
        &s_state.reverb_upsample_buffer[channel][((s_state.reverb_resample_buffer_position >> 1) - 19) & 0x1F];

      GSVector4i srcs = GSVector4i::load<false>(&src[0]);
      GSVector4i acc = GSVector4i::load<true>(&resample_coeff[0]).mul32l(srcs.s16to32());
      acc = acc.add32(GSVector4i::load<true>(&resample_coeff[4]).mul32l(srcs.uph64().s16to32()));
      srcs = GSVector4i::load<false>(&src[8]);
      acc = acc.add32(GSVector4i::load<true>(&resample_coeff[8]).mul32l(srcs.s16to32()));
      acc = acc.add32(GSVector4i::load<true>(&resample_coeff[12]).mul32l(srcs.uph64().s16to32()));
      srcs = GSVector4i::loadl<false>(&src[16]);
      acc = acc.add32(GSVector4i::load<true>(&resample_coeff[16]).mul32l(srcs.s16to32()));

      out[channel] = std::clamp<s32>(acc.addv_s32() >> 14, -32768, 32767);
    }
  }
  else
  {
    const size_t idx = (((s_state.reverb_resample_buffer_position >> 1) - 19) & 0x1F) + 9;
    for (unsigned lr = 0; lr < 2; lr++)
      out[lr] = s_state.reverb_upsample_buffer[lr][idx];
  }

  s_state.reverb_resample_buffer_position = (s_state.reverb_resample_buffer_position + 1) & 0x3F;

  s_state.last_reverb_output[0] = *left_out = ApplyVolume(out[0], s_state.reverb_registers.vLOUT);
  s_state.last_reverb_output[1] = *right_out = ApplyVolume(out[1], s_state.reverb_registers.vROUT);

#ifdef SPU_DUMP_ALL_VOICES
  if (s_state.s_voice_dump_writers[NUM_VOICES])
  {
    const s16 dump_samples[2] = {static_cast<s16>(Clamp16(s_state.last_reverb_output[0])),
                                 static_cast<s16>(Clamp16(s_state.last_reverb_output[1]))};
    s_state.s_voice_dump_writers[NUM_VOICES]->WriteFrames(dump_samples, 1);
  }
#endif
}

void SPU::Execute(void* param, TickCount ticks, TickCount ticks_late)
{
  u32 remaining_frames;
  if (g_settings.cpu_overclock_active)
  {
    // (X * D) / N / 768 -> (X * D) / (N * 768)
    const u64 num =
      (static_cast<u64>(ticks) * g_settings.cpu_overclock_denominator) + static_cast<u32>(s_state.ticks_carry);
    remaining_frames = static_cast<u32>(num / s_state.cpu_tick_divider);
    s_state.ticks_carry = static_cast<TickCount>(num % s_state.cpu_tick_divider);
  }
  else
  {
    remaining_frames = static_cast<u32>((ticks + s_state.ticks_carry) / SYSCLK_TICKS_PER_SPU_TICK);
    s_state.ticks_carry = (ticks + s_state.ticks_carry) % SYSCLK_TICKS_PER_SPU_TICK;
  }

  while (remaining_frames > 0)
  {
    s16* output_frame_start;
    u32 output_frame_space = remaining_frames;
    if (!s_state.audio_output_muted) [[likely]]
    {
      output_frame_space = remaining_frames;
      s_state.audio_stream->BeginWrite(&output_frame_start, &output_frame_space);
    }
    else
    {
      // dummy space for writing samples when using runahead
      output_frame_start = s_muted_output_buffer.data();
      output_frame_space = std::min(static_cast<u32>(s_muted_output_buffer.size() / 2), remaining_frames);
    }

    s16* output_frame = output_frame_start;
    const u32 frames_in_this_batch = std::min(remaining_frames, output_frame_space);
    for (u32 i = 0; i < frames_in_this_batch; i++)
    {
      s32 left_sum = 0;
      s32 right_sum = 0;
      s32 reverb_in_left = 0;
      s32 reverb_in_right = 0;

      u32 reverb_on_register = s_state.reverb_on_register;

      for (u32 voice = 0; voice < NUM_VOICES; voice++)
      {
        const auto [left, right] = SampleVoice(voice);
        left_sum += left;
        right_sum += right;

        if (reverb_on_register & 1u)
        {
          reverb_in_left += left;
          reverb_in_right += right;
        }
        reverb_on_register >>= 1;
      }

      if (!s_state.SPUCNT.mute_n)
      {
        left_sum = 0;
        right_sum = 0;
        reverb_in_left = 0;
        reverb_in_right = 0;
      }

      // Update noise once per frame.
      UpdateNoise();

      // Mix in CD audio.
      const auto [cd_audio_left, cd_audio_right] = CDROM::GetAudioFrame();
      if (s_state.SPUCNT.cd_audio_enable)
      {
        const s32 cd_audio_volume_left = ApplyVolume(s32(cd_audio_left), s_state.cd_audio_volume_left);
        const s32 cd_audio_volume_right = ApplyVolume(s32(cd_audio_right), s_state.cd_audio_volume_right);

        left_sum += cd_audio_volume_left;
        right_sum += cd_audio_volume_right;

        if (s_state.SPUCNT.cd_audio_reverb)
        {
          reverb_in_left += cd_audio_volume_left;
          reverb_in_right += cd_audio_volume_right;
        }
      }

      // Compute reverb.
      s32 reverb_out_left, reverb_out_right;
      ProcessReverb(Clamp16(reverb_in_left), Clamp16(reverb_in_right), &reverb_out_left, &reverb_out_right);

      // Mix in reverb.
      left_sum += reverb_out_left;
      right_sum += reverb_out_right;

      // Apply main volume after clamping. A maximum volume should not overflow here because both are 16-bit values.
      *(output_frame++) = static_cast<s16>(ApplyVolume(Clamp16(left_sum), s_state.main_volume_left.current_level));
      *(output_frame++) = static_cast<s16>(ApplyVolume(Clamp16(right_sum), s_state.main_volume_right.current_level));
      s_state.main_volume_left.Tick();
      s_state.main_volume_right.Tick();

      // Write to capture buffers.
      WriteToCaptureBuffer(0, cd_audio_left);
      WriteToCaptureBuffer(1, cd_audio_right);
      WriteToCaptureBuffer(2, static_cast<s16>(Clamp16(s_state.voices[1].last_volume)));
      WriteToCaptureBuffer(3, static_cast<s16>(Clamp16(s_state.voices[3].last_volume)));
      IncrementCaptureBufferPosition();

      // Key off/on voices after the first frame.
      if (i == 0 && (s_state.key_off_register != 0 || s_state.key_on_register != 0))
      {
        u32 key_off_register = s_state.key_off_register;
        s_state.key_off_register = 0;

        u32 key_on_register = s_state.key_on_register;
        s_state.key_on_register = 0;

        for (u32 voice = 0; voice < NUM_VOICES; voice++)
        {
          if (key_off_register & 1u)
            s_state.voices[voice].KeyOff();
          key_off_register >>= 1;

          if (key_on_register & 1u)
          {
            s_state.endx_register &= ~(1u << voice);
            s_state.voices[voice].KeyOn();
          }
          key_on_register >>= 1;
        }
      }
    }

#ifndef __ANDROID__
    if (MediaCapture* cap = System::GetMediaCapture(); cap && !s_state.audio_output_muted) [[unlikely]]
    {
      if (!cap->DeliverAudioFrames(output_frame_start, frames_in_this_batch))
        System::StopMediaCapture();
    }
#endif

    if (!s_state.audio_output_muted) [[likely]]
      s_state.audio_stream->EndWrite(frames_in_this_batch);
    remaining_frames -= frames_in_this_batch;
  }
}

void SPU::UpdateEventInterval()
{
  // Don't generate more than the audio buffer since in a single slice, otherwise we'll both overflow the buffers when
  // we do write it, and the audio thread will underflow since it won't have enough data it the game isn't messing with
  // the SPU state.
  const u32 max_slice_frames = s_state.audio_stream->GetBufferSize();

  // TODO: Make this predict how long until the interrupt will be hit instead...
  const u32 interval = (s_state.SPUCNT.enable && s_state.SPUCNT.irq9_enable) ? 1 : max_slice_frames;
  const TickCount interval_ticks = static_cast<TickCount>(interval) * s_state.cpu_ticks_per_spu_tick;
  if (s_state.tick_event.IsActive() && s_state.tick_event.GetInterval() == interval_ticks)
    return;

  // Ticks remaining before execution should be retained, just adjust the interval/downcount.
  const TickCount new_downcount = interval_ticks - s_state.ticks_carry;
  s_state.tick_event.SetInterval(interval_ticks);
  s_state.tick_event.Schedule(new_downcount);
}

void SPU::DrawDebugStateWindow(float scale)
{
  static const ImVec4 active_color{1.0f, 1.0f, 1.0f, 1.0f};
  static const ImVec4 inactive_color{0.4f, 0.4f, 0.4f, 1.0f};

  // status
  if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static constexpr std::array<const char*, 4> transfer_modes = {
      {"Transfer Stopped", "Manual Write", "DMA Write", "DMA Read"}};
    const std::array<float, 6> offsets = {
      {100.0f * scale, 200.0f * scale, 300.0f * scale, 420.0f * scale, 500.0f * scale, 600.0f * scale}};

    ImGui::Text("Control: ");
    ImGui::SameLine(offsets[0]);
    ImGui::TextColored(s_state.SPUCNT.enable ? active_color : inactive_color, "SPU Enable");
    ImGui::SameLine(offsets[1]);
    ImGui::TextColored(s_state.SPUCNT.mute_n ? inactive_color : active_color, "Mute SPU");
    ImGui::SameLine(offsets[2]);
    ImGui::TextColored(s_state.SPUCNT.external_audio_enable ? active_color : inactive_color, "External Audio");
    ImGui::SameLine(offsets[3]);
    ImGui::TextColored(s_state.SPUCNT.ram_transfer_mode != RAMTransferMode::Stopped ? active_color : inactive_color,
                       "%s", transfer_modes[static_cast<u8>(s_state.SPUCNT.ram_transfer_mode.GetValue())]);

    ImGui::Text("Status: ");
    ImGui::SameLine(offsets[0]);
    ImGui::TextColored(s_state.SPUSTAT.irq9_flag ? active_color : inactive_color, "IRQ9");
    ImGui::SameLine(offsets[1]);
    ImGui::TextColored(s_state.SPUSTAT.dma_request ? active_color : inactive_color, "DMA Request");
    ImGui::SameLine(offsets[2]);
    ImGui::TextColored(s_state.SPUSTAT.dma_read_request ? active_color : inactive_color, "DMA Read");
    ImGui::SameLine(offsets[3]);
    ImGui::TextColored(s_state.SPUSTAT.dma_write_request ? active_color : inactive_color, "DMA Write");
    ImGui::SameLine(offsets[4]);
    ImGui::TextColored(s_state.SPUSTAT.transfer_busy ? active_color : inactive_color, "Transfer Busy");
    ImGui::SameLine(offsets[5]);
    ImGui::TextColored(s_state.SPUSTAT.second_half_capture_buffer ? active_color : inactive_color,
                       "Second Capture Buffer");

    ImGui::Text("Interrupt: ");
    ImGui::SameLine(offsets[0]);
    ImGui::TextColored(s_state.SPUCNT.irq9_enable ? active_color : inactive_color,
                       s_state.SPUCNT.irq9_enable ? "Enabled @ 0x%04X (actual 0x%08X)" :
                                                    "Disabled @ 0x%04X (actual 0x%08X)",
                       s_state.irq_address, (ZeroExtend32(s_state.irq_address) * 8) & RAM_MASK);

    ImGui::Text("Volume: ");
    ImGui::SameLine(offsets[0]);
    ImGui::Text("Left: %d%%", ApplyVolume(100, s_state.main_volume_left.current_level));
    ImGui::SameLine(offsets[1]);
    ImGui::Text("Right: %d%%", ApplyVolume(100, s_state.main_volume_right.current_level));

    ImGui::Text("CD Audio: ");
    ImGui::SameLine(offsets[0]);
    ImGui::TextColored(s_state.SPUCNT.cd_audio_enable ? active_color : inactive_color,
                       s_state.SPUCNT.cd_audio_enable ? "Enabled" : "Disabled");
    ImGui::SameLine(offsets[1]);
    ImGui::TextColored(s_state.SPUCNT.cd_audio_enable ? active_color : inactive_color, "Left Volume: %d%%",
                       ApplyVolume(100, s_state.cd_audio_volume_left));
    ImGui::SameLine(offsets[3]);
    ImGui::TextColored(s_state.SPUCNT.cd_audio_enable ? active_color : inactive_color, "Right Volume: %d%%",
                       ApplyVolume(100, s_state.cd_audio_volume_left));

    ImGui::Text("Transfer FIFO: ");
    ImGui::SameLine(offsets[0]);
    ImGui::TextColored(s_state.transfer_event.IsActive() ? active_color : inactive_color, "%u halfwords (%u bytes)",
                       s_state.transfer_fifo.GetSize(), s_state.transfer_fifo.GetSize() * 2);
  }

  // draw voice states
  if (ImGui::CollapsingHeader("Voice State", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static constexpr u32 NUM_COLUMNS = 12;

    ImGui::Columns(NUM_COLUMNS);

    // headers
    static constexpr std::array<const char*, NUM_COLUMNS> column_titles = {
      {"#", "InterpIndex", "SampleIndex", "CurAddr", "StartAddr", "RepeatAddr", "SampleRate", "VolLeft", "VolRight",
       "ADSRPhase", "ADSRVol", "ADSRTicks"}};
    static constexpr std::array<const char*, 5> adsr_phases = {{"Off", "Attack", "Decay", "Sustain", "Release"}};
    for (u32 i = 0; i < NUM_COLUMNS; i++)
    {
      ImGui::TextUnformatted(column_titles[i]);
      ImGui::NextColumn();
    }

    // states
    for (u32 voice_index = 0; voice_index < NUM_VOICES; voice_index++)
    {
      const Voice& v = s_state.voices[voice_index];
      ImVec4 color = v.IsOn() ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
      ImGui::TextColored(color, "%u", ZeroExtend32(voice_index));
      ImGui::NextColumn();
      if (IsVoiceNoiseEnabled(voice_index))
        ImGui::TextColored(color, "NOISE");
      else
        ImGui::TextColored(color, "%u", ZeroExtend32(v.counter.interpolation_index.GetValue()));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%u", ZeroExtend32(v.counter.sample_index.GetValue()));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%04X", ZeroExtend32(v.current_address));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%04X", ZeroExtend32(v.regs.adpcm_start_address));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%04X", ZeroExtend32(v.regs.adpcm_repeat_address));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%.2f", (float(v.regs.adpcm_sample_rate) / 4096.0f) * 44100.0f);
      ImGui::NextColumn();
      ImGui::TextColored(color, "%d%%", ApplyVolume(100, v.left_volume.current_level));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%d%%", ApplyVolume(100, v.right_volume.current_level));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%s", adsr_phases[static_cast<u8>(v.adsr_phase)]);
      ImGui::NextColumn();
      ImGui::TextColored(color, "%d%%", ApplyVolume(100, v.regs.adsr_volume));
      ImGui::NextColumn();
      ImGui::TextColored(color, "%d", v.adsr_envelope.counter);
      ImGui::NextColumn();
    }

    ImGui::Columns(1);
  }

  if (ImGui::CollapsingHeader("Reverb", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::TextColored(s_state.SPUCNT.reverb_master_enable ? active_color : inactive_color, "Master Enable: %s",
                       s_state.SPUCNT.reverb_master_enable ? "Yes" : "No");
    ImGui::Text("Voices Enabled: ");

    for (u32 i = 0; i < NUM_VOICES; i++)
    {
      ImGui::SameLine(0.0f, 16.0f);

      const bool active = IsVoiceReverbEnabled(i);
      ImGui::TextColored(active ? active_color : inactive_color, "%u", i);
    }

    ImGui::TextColored(s_state.SPUCNT.cd_audio_reverb ? active_color : inactive_color, "CD Audio Enable: %s",
                       s_state.SPUCNT.cd_audio_reverb ? "Yes" : "No");

    ImGui::TextColored(s_state.SPUCNT.external_audio_reverb ? active_color : inactive_color,
                       "External Audio Enable: %s", s_state.SPUCNT.external_audio_reverb ? "Yes" : "No");

    ImGui::Text("Base Address: 0x%08X (%04X)", s_state.reverb_base_address, s_state.reverb_registers.mBASE);
    ImGui::Text("Current Address: 0x%08X", s_state.reverb_current_address);
    ImGui::Text("Current Amplitude: Input (%d, %d) Output (%d, %d)", s_state.last_reverb_input[0],
                s_state.last_reverb_input[1], s_state.last_reverb_output[0], s_state.last_reverb_output[1]);
    ImGui::Text("Output Volume: Left %d%% Right %d%%", ApplyVolume(100, s_state.reverb_registers.vLOUT),
                ApplyVolume(100, s_state.reverb_registers.vROUT));

    ImGui::Text("Pitch Modulation: ");
    for (u32 i = 1; i < NUM_VOICES; i++)
    {
      ImGui::SameLine(0.0f, 16.0f);

      const bool active = IsPitchModulationEnabled(i);
      ImGui::TextColored(active ? active_color : inactive_color, "%u", i);
    }
  }

  if (ImGui::CollapsingHeader("Hacks", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (ImGui::Button("Key Off All Voices"))
    {
      for (u32 i = 0; i < NUM_VOICES; i++)
      {
        s_state.voices[i].KeyOff();
        s_state.voices[i].adsr_envelope.counter = 0;
        s_state.voices[i].regs.adsr_volume = 0;
      }
    }
  }
}
