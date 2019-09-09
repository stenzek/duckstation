#pragma once
#include "types.h"

class StateWrapper;
class String;

class DisplayTiming
{
public:
  DisplayTiming();

  // H/V frequencies are valid?
  bool IsValid() const { return m_valid; }

  // Enables the clock at the specified start time.
  bool IsClockEnabled() const { return m_clock_enable; }
  void SetClockEnable(bool enable) { m_clock_enable = enable; }
  void ResetClock(SimulationTime start_time);

  // Returns the number of ticks since the clock was enabled.
  SimulationTime GetTime(SimulationTime time) const;

  // Returns the number of ticks elapsed in the current frame.
  s32 GetTimeInFrame(SimulationTime time) const;

  // Accessors.
  s32 GetHorizontalVisible() const { return m_horizontal_visible; }
  s32 GetHorizontalFrontPorch() const { return m_horizontal_front_porch; }
  s32 GetHorizontalSyncLength() const { return m_horizontal_sync_length; }
  s32 GetHorizontalBackPorch() const { return m_horizontal_back_porch; }
  s32 GetHorizontalTotal() const { return m_horizontal_total; }
  s32 GetVerticalVisible() const { return m_vertical_visible; }
  s32 GetVerticalFrontPorch() const { return m_vertical_front_porch; }
  s32 GetVerticalSyncLength() const { return m_vertical_sync_length; }
  s32 GetVerticallBackPorch() const { return m_vertical_back_porch; }
  s32 GetVerticalTotal() const { return m_vertical_total; }
  double GetPixelClock() const { return m_pixel_clock; }
  double GetHorizontalFrequency() const { return m_horizontal_frequency; }
  double GetVerticalFrequency() const { return m_vertical_frequency; }
  s32 GetHorizontalPixelDuration() const { return m_horizontal_pixel_duration; }
  s32 GetHorizontalActiveDuration() const { return m_horizontal_active_duration; }
  s32 GetHorizontalSyncStartTime() const { return m_horizontal_sync_start_time; }
  s32 GetHorizontalSyncEndTime() const { return m_horizontal_sync_end_time; }
  s32 GetHorizontalTotalDuration() const { return m_horizontal_total_duration; }
  s32 GetHorizontalBlankStartTime() const { return m_horizontal_active_duration; }
  s32 GetHorizontalBlankDuration() const { return m_horizontal_total_duration - m_horizontal_active_duration; }
  s32 GetVerticalActiveDuration() const { return m_vertical_active_duration; }
  s32 GetVerticalSyncStartTime() const { return m_vertical_sync_start_time; }
  s32 GetVerticalSyncEndTime() const { return m_vertical_sync_end_time; }
  s32 GetVerticalTotalDuration() const { return m_vertical_total_duration; }
  s32 GetVerticalBlankStartTime() const { return m_vertical_active_duration; }
  s32 GetVerticalBlankDuration() const { return m_vertical_total_duration - m_vertical_active_duration; }

  // Setting horizontal timing based on pixels and clock.
  void SetPixelClock(double clock);
  void SetHorizontalVisible(s32 visible);
  void SetHorizontalSyncRange(s32 start, s32 end);
  void SetHorizontalSyncLength(s32 start, s32 length);
  void SetHorizontalBackPorch(s32 bp);
  void SetHorizontalTotal(s32 total);
  void SetVerticalVisible(s32 visible);
  void SetVerticalSyncRange(s32 start, s32 end);
  void SetVerticalSyncLength(s32 start, s32 length);
  void SetVerticalBackPorch(s32 bp);
  void SetVerticalTotal(s32 total);

  // Gets the timing state for the specified time point.
  struct Snapshot
  {
    u32 current_line;
    u32 current_pixel;
    bool display_active; // visible part
    bool in_horizontal_blank;
    bool in_vertical_blank;
    bool hsync_active;
    bool vsync_active;
  };
  Snapshot GetSnapshot(SimulationTime time) const;

  // Shorter versions of the above.
  bool IsDisplayActive(SimulationTime time) const;
  bool InVerticalBlank(SimulationTime time) const;
  bool InHorizontalSync(SimulationTime time) const;
  bool InVerticalSync(SimulationTime time) const;
  u32 GetCurrentLine(SimulationTime time) const;
  SimulationTime GetTimeUntilVSync(SimulationTime time) const;

  // Returns the amount of time until the next vertical blank starts.
  SimulationTime GetTimeUntilVBlank(SimulationTime time) const;

  // Writes frequency information to the log.
  void ToString(String* str) const;

  // Tests whether frequencies and dimensions match.
  bool FrequenciesMatch(const DisplayTiming& timing) const;

  // Serialization.
  bool DoState(StateWrapper& sw);
  void Reset();

  // Copy operator.
  DisplayTiming& operator=(const DisplayTiming& timing);

  // TODO: clock update to prevent wrap-around.

private:
  void UpdateHorizontalFrequency();
  void UpdateVerticalFrequency();
  void UpdateValid();

  SimulationTime m_clock_start_time = 0;

  // Set
  s32 m_horizontal_visible = 0;
  s32 m_horizontal_front_porch = 0;
  s32 m_horizontal_sync_length = 0;
  s32 m_horizontal_back_porch = 0;
  s32 m_vertical_visible = 0;
  s32 m_vertical_front_porch = 0;
  s32 m_vertical_sync_length = 0;
  s32 m_vertical_back_porch = 0;

  // Computed. End values are exclusive.
  s32 m_horizontal_total = 0;
  s32 m_vertical_total = 0;

  double m_pixel_clock = 0.0;
  double m_horizontal_frequency = 0.0f;
  double m_vertical_frequency = 0.0f;

  // TODO: Make these doubles?
  s32 m_horizontal_pixel_duration = 0;
  s32 m_horizontal_active_duration = 0;
  s32 m_horizontal_sync_start_time = 0;
  s32 m_horizontal_sync_end_time = 0;
  s32 m_horizontal_total_duration = 0;
  s32 m_vertical_active_duration = 0;
  s32 m_vertical_sync_start_time = 0;
  s32 m_vertical_sync_end_time = 0;
  s32 m_vertical_total_duration = 0;

  bool m_clock_enable = false;
  bool m_valid = false;
};
