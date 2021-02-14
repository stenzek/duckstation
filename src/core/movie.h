#pragma once
#include "types.h"
#include <string>
#include <vector>
#include <memory>

class Movie
{
public:
  ~Movie();

  static std::unique_ptr<Movie> Load(const char* filename);

  ALWAYS_INLINE u32 GetNumFrames() const { return m_num_frames; }
  ALWAYS_INLINE bool AtEnd() const { return (m_current_frame == m_num_frames); }

  void Rewind();
  void NextFrame();

private:
  enum : u32
  {
    START_OF_MOVIE = 0xFFFFFFFFu
  };

  using InputValue = s8;

  struct InputMapping
  {
    enum class Type
    {
      None,
      DiscSelect,
      DiscOpen,
      DiscClose,
      DiscReset,
      Button,
      Axis,
    };

    Type type;
    u32 controller_index;
    s32 axis_or_button_code;
  };

  Movie();

  bool LoadBk2(const char* filename);
  bool LoadBk2Txt(const std::string& data);
  void ApplyInputsForFrame(u32 frame_number);

  std::vector<InputMapping> m_input_mappings;
  std::vector<InputValue> m_input_values;
  u32 m_num_frames = 0;
  u32 m_current_frame = START_OF_MOVIE;
};