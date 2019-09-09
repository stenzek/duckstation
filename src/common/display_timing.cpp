#include "display_timing.h"
#include "YBaseLib/String.h"
#include "common/state_wrapper.h"

DisplayTiming::DisplayTiming() = default;

void DisplayTiming::ResetClock(SimulationTime start_time)
{
  m_clock_start_time = start_time;
}

SimulationTime DisplayTiming::GetTime(SimulationTime time) const
{
  return GetSimulationTimeDifference(m_clock_start_time, time);
}

s32 DisplayTiming::GetTimeInFrame(SimulationTime time) const
{
  return static_cast<s32>(GetTime(time) % m_vertical_total_duration);
}

void DisplayTiming::SetPixelClock(double clock)
{
  m_pixel_clock = clock;
  UpdateHorizontalFrequency();
  UpdateVerticalFrequency();
}

void DisplayTiming::SetHorizontalVisible(s32 visible)
{
  m_horizontal_visible = visible;
  UpdateHorizontalFrequency();
}

void DisplayTiming::SetHorizontalSyncRange(s32 start, s32 end)
{
  Assert(start <= end);
  m_horizontal_front_porch = start - m_horizontal_visible;
  m_horizontal_sync_length = end - start + 1;
  UpdateHorizontalFrequency();
}

void DisplayTiming::SetHorizontalSyncLength(s32 start, s32 length)
{
  m_horizontal_front_porch = start - m_horizontal_visible;
  m_horizontal_sync_length = length;
  UpdateHorizontalFrequency();
}

void DisplayTiming::SetHorizontalBackPorch(s32 bp)
{
  m_horizontal_back_porch = bp;
  UpdateHorizontalFrequency();
}

void DisplayTiming::SetHorizontalTotal(s32 total)
{
  m_horizontal_back_porch = total - (m_horizontal_visible + m_horizontal_front_porch + m_horizontal_sync_length);
  UpdateHorizontalFrequency();
}

void DisplayTiming::SetVerticalVisible(s32 visible)
{
  m_vertical_visible = visible;
  UpdateVerticalFrequency();
}

void DisplayTiming::SetVerticalSyncRange(s32 start, s32 end)
{
  Assert(start <= end);
  m_vertical_front_porch = start - m_vertical_visible;
  m_vertical_sync_length = end - start + 1;
  UpdateVerticalFrequency();
}

void DisplayTiming::SetVerticalSyncLength(s32 start, s32 length)
{
  m_vertical_front_porch = start - m_vertical_visible;
  m_vertical_sync_length = length;
  UpdateVerticalFrequency();
}

void DisplayTiming::SetVerticalBackPorch(s32 bp)
{
  m_vertical_back_porch = bp;
  UpdateVerticalFrequency();
}

void DisplayTiming::SetVerticalTotal(s32 total)
{
  m_vertical_back_porch = total - (m_vertical_visible + m_vertical_front_porch + m_vertical_sync_length);
  UpdateVerticalFrequency();
}

DisplayTiming::Snapshot DisplayTiming::GetSnapshot(SimulationTime time) const
{
  Snapshot ss;
  if (m_clock_enable && IsValid())
  {
    const s32 time_in_frame = GetTimeInFrame(time);
    const s32 line_number = time_in_frame / m_horizontal_total_duration;
    const s32 time_in_line = time_in_frame % m_horizontal_total_duration;
    ss.current_line = static_cast<u32>(line_number);
    ss.current_pixel = static_cast<u32>(time_in_line / m_horizontal_pixel_duration);
    ss.in_vertical_blank = (time_in_frame >= m_vertical_active_duration);
    ss.in_horizontal_blank = (!ss.in_vertical_blank && (time_in_line >= m_horizontal_sync_start_time &&
                                                        time_in_line < m_horizontal_sync_end_time));
    ss.vsync_active = (time_in_frame >= m_vertical_sync_start_time && line_number < m_vertical_sync_end_time);
    ss.hsync_active =
      (!ss.vsync_active && (time_in_line >= m_horizontal_sync_start_time && time_in_line < m_horizontal_sync_end_time));
    ss.display_active = !(ss.in_horizontal_blank | ss.in_vertical_blank);
  }
  else
  {
    ss.current_line = 0;
    ss.current_pixel = 0;
    ss.display_active = false;
    ss.in_horizontal_blank = false;
    ss.in_vertical_blank = false;
    ss.hsync_active = false;
    ss.vsync_active = false;
  }
  return ss;
}

bool DisplayTiming::IsDisplayActive(SimulationTime time) const
{
  if (!m_clock_enable || !IsValid())
    return false;

  const s32 time_in_frame = GetTimeInFrame(time);
  return (time_in_frame < m_vertical_active_duration &&
          (time_in_frame % m_horizontal_total_duration) < m_horizontal_active_duration);
}

bool DisplayTiming::InVerticalBlank(SimulationTime time) const
{
  if (!m_clock_enable || !IsValid())
    return false;

  const s32 time_in_frame = GetTimeInFrame(time);
  return (time_in_frame >= m_vertical_active_duration);
}

bool DisplayTiming::InHorizontalSync(SimulationTime time) const
{
  if (!m_clock_enable || !IsValid())
    return false;

  const s32 time_in_frame = GetTimeInFrame(time);
  if (time_in_frame >= m_vertical_sync_start_time && time_in_frame < m_vertical_sync_end_time)
  {
    // In vsync.
    return false;
  }

  const s32 time_in_line = time_in_frame % m_horizontal_total_duration;
  return (time_in_line >= m_horizontal_sync_start_time && time_in_frame < m_horizontal_sync_end_time);
}

bool DisplayTiming::InVerticalSync(SimulationTime time) const
{
  const s32 time_in_frame = GetTimeInFrame(time);
  return (time_in_frame >= m_vertical_sync_start_time && time_in_frame < m_vertical_sync_end_time);
}

u32 DisplayTiming::GetCurrentLine(SimulationTime time) const
{
  if (!m_clock_enable || !IsValid())
    return 0;

  const s32 time_in_frame = GetTimeInFrame(time);
  return static_cast<u32>(time_in_frame / m_horizontal_total_duration);
}

SimulationTime DisplayTiming::GetTimeUntilVSync(SimulationTime time) const
{
  if (!m_clock_enable || !IsValid())
    return 0;

  const s32 time_in_frame = GetTimeInFrame(time);
  if (time_in_frame < m_vertical_sync_start_time)
    return m_vertical_sync_start_time - time_in_frame;
  else
    return (m_vertical_total_duration - time_in_frame) + m_vertical_sync_start_time;
}

SimulationTime DisplayTiming::GetTimeUntilVBlank(SimulationTime time) const
{
  if (!m_clock_enable || !IsValid())
    return 0;

  const s32 time_in_frame = GetTimeInFrame(time);
  if (time_in_frame >= m_vertical_active_duration)
    return ((m_vertical_total_duration - time_in_frame) + m_vertical_active_duration);
  else
    return (m_vertical_active_duration - time_in_frame);
}

void DisplayTiming::ToString(String* str) const
{
  const s32 horizontal_sync_start = m_horizontal_visible + m_horizontal_front_porch;
  const s32 vertical_sync_start = m_vertical_visible + m_vertical_front_porch;
  str->Format("%dx%d | %.3f KHz, %u Total, %d-%d Sync | %.3fhz, %d Total, %d-%d Sync", m_horizontal_visible,
              m_vertical_visible, m_horizontal_frequency / 1000.0, m_horizontal_total, horizontal_sync_start,
              horizontal_sync_start + m_horizontal_sync_length, m_vertical_frequency, m_vertical_total,
              vertical_sync_start, vertical_sync_start + m_vertical_sync_length);
}

bool DisplayTiming::FrequenciesMatch(const DisplayTiming& timing) const
{
  return std::tie(m_pixel_clock, m_horizontal_visible, m_horizontal_front_porch, m_horizontal_sync_length,
                  m_horizontal_back_porch, m_horizontal_frequency, m_vertical_visible, m_vertical_front_porch,
                  m_vertical_sync_length, m_vertical_back_porch, m_vertical_frequency) ==
         std::tie(timing.m_pixel_clock, timing.m_horizontal_visible, timing.m_horizontal_front_porch,
                  timing.m_horizontal_sync_length, timing.m_horizontal_back_porch, timing.m_horizontal_frequency,
                  timing.m_vertical_visible, timing.m_vertical_front_porch, timing.m_vertical_sync_length,
                  timing.m_vertical_back_porch, timing.m_vertical_frequency);
}

bool DisplayTiming::DoState(StateWrapper& sw)
{
  sw.Do(&m_clock_start_time);
  sw.Do(&m_horizontal_visible);
  sw.Do(&m_horizontal_front_porch);
  sw.Do(&m_horizontal_sync_length);
  sw.Do(&m_horizontal_back_porch);
  sw.Do(&m_vertical_visible);
  sw.Do(&m_vertical_front_porch);
  sw.Do(&m_vertical_sync_length);
  sw.Do(&m_vertical_back_porch);
  sw.Do(&m_horizontal_total);
  sw.Do(&m_vertical_total);
  sw.Do(&m_pixel_clock);
  sw.Do(&m_horizontal_frequency);
  sw.Do(&m_vertical_frequency);
  sw.Do(&m_horizontal_pixel_duration);
  sw.Do(&m_horizontal_active_duration);
  sw.Do(&m_horizontal_sync_start_time);
  sw.Do(&m_horizontal_sync_end_time);
  sw.Do(&m_horizontal_total_duration);
  sw.Do(&m_vertical_active_duration);
  sw.Do(&m_vertical_sync_start_time);
  sw.Do(&m_vertical_sync_end_time);
  sw.Do(&m_vertical_total_duration);
  sw.Do(&m_clock_enable);
  sw.Do(&m_valid);
  return !sw.HasError();
}

void DisplayTiming::Reset()
{
  m_clock_start_time = 0;

  m_horizontal_visible = 0;
  m_horizontal_front_porch = 0;
  m_horizontal_sync_length = 0;
  m_horizontal_back_porch = 0;
  m_vertical_visible = 0;
  m_vertical_front_porch = 0;
  m_vertical_sync_length = 0;
  m_vertical_back_porch = 0;

  m_horizontal_total = 0;
  m_vertical_total = 0;

  m_pixel_clock = 0.0;
  m_horizontal_frequency = 0.0f;
  m_vertical_frequency = 0.0f;

  m_horizontal_pixel_duration = 0;
  m_horizontal_active_duration = 0;
  m_horizontal_sync_start_time = 0;
  m_horizontal_sync_end_time = 0;
  m_horizontal_total_duration = 0;
  m_vertical_active_duration = 0;
  m_vertical_sync_start_time = 0;
  m_vertical_sync_end_time = 0;
  m_vertical_total_duration = 0;

  m_clock_enable = false;
  m_valid = false;
}

DisplayTiming& DisplayTiming::operator=(const DisplayTiming& timing)
{
  m_clock_start_time = timing.m_clock_start_time;
  m_horizontal_visible = timing.m_horizontal_visible;
  m_horizontal_front_porch = timing.m_horizontal_front_porch;
  m_horizontal_sync_length = timing.m_horizontal_sync_length;
  m_horizontal_back_porch = timing.m_horizontal_back_porch;
  m_vertical_visible = timing.m_vertical_visible;
  m_vertical_front_porch = timing.m_vertical_front_porch;
  m_vertical_sync_length = timing.m_vertical_sync_length;
  m_vertical_back_porch = timing.m_vertical_back_porch;
  m_horizontal_total = timing.m_horizontal_total;
  m_vertical_total = timing.m_vertical_total;
  m_pixel_clock = timing.m_pixel_clock;
  m_horizontal_frequency = timing.m_horizontal_frequency;
  m_vertical_frequency = timing.m_vertical_frequency;
  m_horizontal_pixel_duration = timing.m_horizontal_pixel_duration;
  m_horizontal_active_duration = timing.m_horizontal_active_duration;
  m_horizontal_sync_start_time = timing.m_horizontal_sync_start_time;
  m_horizontal_sync_end_time = timing.m_horizontal_sync_end_time;
  m_horizontal_total_duration = timing.m_horizontal_total_duration;
  m_vertical_active_duration = timing.m_vertical_active_duration;
  m_vertical_sync_start_time = timing.m_vertical_sync_start_time;
  m_vertical_sync_end_time = timing.m_vertical_sync_end_time;
  m_vertical_total_duration = timing.m_vertical_total_duration;
  m_clock_enable = timing.m_clock_enable;
  m_valid = timing.m_valid;
  return *this;
}

void DisplayTiming::UpdateHorizontalFrequency()
{
  if (m_pixel_clock == 0.0 || m_horizontal_visible <= 0 || m_horizontal_front_porch < 0 ||
      m_horizontal_sync_length < 0 || m_horizontal_back_porch < 0)
  {
    m_horizontal_total = 0;
    m_horizontal_frequency = 0.0;
    m_horizontal_active_duration = 0;
    m_horizontal_sync_start_time = 0;
    m_horizontal_sync_end_time = 0;
    m_horizontal_total_duration = 0;
    UpdateVerticalFrequency();
    return;
  }

  m_horizontal_total =
    m_horizontal_visible + m_horizontal_front_porch + m_horizontal_sync_length + m_horizontal_back_porch;

  const double pixel_period = 1.0 / m_pixel_clock;
  const double active_duration_s = pixel_period * static_cast<double>(m_horizontal_visible);
  const double sync_start_time_s = pixel_period * static_cast<double>(m_horizontal_visible + m_horizontal_front_porch);
  const double sync_end_time_s = sync_start_time_s + (pixel_period * static_cast<double>(m_horizontal_sync_length));
  const double total_duration_s = pixel_period * static_cast<double>(m_horizontal_total);

  m_horizontal_frequency = m_pixel_clock / static_cast<double>(m_horizontal_total);
  m_horizontal_pixel_duration = static_cast<u32>(1000000000.0 * pixel_period);
  m_horizontal_active_duration = static_cast<u32>(1000000000.0 * active_duration_s);
  m_horizontal_sync_start_time = static_cast<u32>(1000000000.0 * sync_start_time_s);
  m_horizontal_sync_end_time = static_cast<u32>(1000000000.0 * sync_end_time_s);
  m_horizontal_total_duration = static_cast<u32>(1000000000.0 * total_duration_s);
  UpdateVerticalFrequency();
}

void DisplayTiming::UpdateVerticalFrequency()
{
  if (m_vertical_visible <= 0 || m_vertical_front_porch < 0 || m_vertical_sync_length < 0 || m_vertical_back_porch < 0)
  {
    m_vertical_total = 0;
    m_vertical_frequency = 0;
    m_vertical_active_duration = 0;
    m_vertical_sync_start_time = 0;
    m_vertical_sync_end_time = 0;
    m_vertical_total_duration = 0;
    UpdateValid();
    return;
  }

  m_vertical_total = m_vertical_visible + m_vertical_front_porch + m_vertical_sync_length + m_vertical_back_porch;
  m_vertical_frequency = m_horizontal_frequency / static_cast<double>(m_vertical_total);
  m_vertical_active_duration = m_horizontal_total_duration * m_vertical_visible;
  m_vertical_sync_start_time = m_horizontal_total_duration * (m_vertical_visible + m_vertical_front_porch);
  m_vertical_sync_end_time = m_vertical_sync_start_time + (m_horizontal_total_duration * m_vertical_sync_length);
  m_vertical_total_duration = m_horizontal_total_duration * m_vertical_total;
  UpdateValid();
}

void DisplayTiming::UpdateValid()
{
  m_valid = (m_horizontal_total_duration > 0 && m_vertical_total_duration > 0);
}
