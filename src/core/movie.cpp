#include "movie.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "controller.h"
#include "pad.h"
#include "settings.h"
#include "system.h"
#include <sstream>
Log_SetChannel(Movie);

Movie::Movie() = default;

Movie::~Movie() = default;

void Movie::Rewind()
{
  m_current_frame = START_OF_MOVIE;
}

void Movie::NextFrame()
{
  if (m_current_frame == m_num_frames)
    return;

  m_current_frame++;
  ApplyInputsForFrame(m_current_frame);
}

void Movie::ApplyInputsForFrame(u32 frame_number)
{
  if (frame_number >= m_num_frames)
    return;

  const std::size_t start_index = frame_number * m_input_mappings.size();
  Assert((start_index + m_input_mappings.size()) <= m_input_values.size());

  std::size_t index = start_index;
  for (const InputMapping& im : m_input_mappings)
  {
    const s8 value = m_input_values[index++];
    switch (im.type)
    {
      case InputMapping::Type::Button:
      {
        Controller* controller = g_pad.GetController(im.controller_index);
        if (controller)
        {
          if (value != 0)
            Log_InfoPrintf("Frame %u button %d down", frame_number + 1, im.axis_or_button_code);

          controller->SetButtonState(im.axis_or_button_code, (value != 0));
        }
      }
      break;
    }
  }
}

std::unique_ptr<Movie> Movie::Load(const char* filename)
{
  std::unique_ptr<Movie> movie = std::unique_ptr<Movie>(new Movie());
  if (!movie->LoadBk2(filename))
    return {};

  return movie;
}

bool Movie::LoadBk2(const char* filename)
{
  std::optional<std::string> data = FileSystem::ReadFileToString(filename);
  if (!data.has_value() || data->empty())
    return false;

  return LoadBk2Txt(data.value());
}

static void SplitBk2InputString(const std::string_view& str, char delim, std::vector<std::string_view>* tokens)
{
  tokens->clear();

  std::string::size_type start_pos = 0;
  std::string::size_type current_pos = 0;
  while (current_pos < str.size())
  {
    if (str[current_pos] == ' ' || str[current_pos] == ',' || str[current_pos] == '|')
    {
      if (current_pos != start_pos)
        tokens->push_back(str.substr(start_pos, current_pos - start_pos));

      current_pos++;
      start_pos = current_pos;
      continue;
    }

    if (str[current_pos] >= '0' && str[current_pos] <= '9')
    {
      current_pos++;
      continue;
    }

    current_pos++;
    tokens->push_back(str.substr(start_pos, current_pos - start_pos));
    start_pos = current_pos;
  }

  if (start_pos != current_pos)
    tokens->push_back(str.substr(start_pos, current_pos - start_pos));
}

bool Movie::LoadBk2Txt(const std::string& data)
{
  std::istringstream iss(data);

  std::string line;
  std::vector<std::string_view> tokens;
  bool in_input_section = false;
  while (std::getline(iss, line))
  {
    StringUtil::TrimWhitespace(line);
    if (line.empty())
      continue;

    if (line == "[Input]")
    {
      in_input_section = true;
      continue;
    }
    else if (line == "[/Input]")
    {
      in_input_section = false;
      break;
    }
    else if (!in_input_section)
    {
      continue;
    }
    else if (StringUtil::StartsWith(line, "LogKey:") && line.size() >= 8)
    {
      // parse key - extra character here for the terminating |
      const std::string_view sv(line.c_str() + 7, line.size() - 8);
      StringUtil::SplitString(sv, '|', &tokens);
      for (const std::string_view& token : tokens)
      {
        InputMapping mapping = {};
        mapping.type = InputMapping::Type::None;

        if (token.size() > 2 && token[0] == 'P' && token[1] >= '1' && token[1] <= '2' && token[2] == ' ')
        {
          mapping.controller_index = token[1] - '1';

          const ControllerType ctype = g_settings.controller_types[mapping.controller_index];
          if (ctype != ControllerType::None)
          {
            const std::string_view& bsv(token.substr(3));
            const Controller::ButtonList blist(Controller::GetButtonNames(ctype));
            for (const auto& it : blist)
            {
              if (bsv == it.first)
              {
                mapping.axis_or_button_code = it.second;
                mapping.type = InputMapping::Type::Button;
                break;
              }
            }

            if (mapping.type == InputMapping::Type::None)
              Log_WarningPrintf("Input '%s' was not mapped", std::string(token).c_str());
          }
        }
        else
        {
          Log_WarningPrintf("Unhandled key token '%s'", std::string(token).c_str());
        }

        m_input_mappings.push_back(mapping);
      }
    }
    else if (line.size() >= 2 && line.front() == '|' && line.back() == '|')
    {
      if (m_input_mappings.empty())
      {
        Log_ErrorPrintf("Mappings not set");
        return false;
      }

      // parse line
      const std::string_view sv(line.c_str() + 1, line.size() - 2);
      SplitBk2InputString(sv, ',', &tokens);
      if (tokens.size() != m_input_mappings.size())
      {
        Log_ErrorPrintf("Incorrect number of mappings: got %zu expected %zu", tokens.size(), m_input_mappings.size());
        return false;
      }

      for (const std::string_view& token : tokens)
      {
        const std::string_view& trimmed_token(StringUtil::TrimWhitespace(token));
        if (trimmed_token.empty())
        {
          // ???
          m_input_values.push_back(0);
        }
        else if (trimmed_token[0] == '.')
        {
          // button off
          m_input_values.push_back(0);
        }
        else if (trimmed_token[0] < '0' || trimmed_token[0] > '1')
        {
          // button on
          m_input_values.push_back(1);
        }
        else
        {
          const s32 value = StringUtil::FromChars<s32>(trimmed_token).value_or(0);
          m_input_values.push_back(static_cast<s8>(std::clamp<s32>(value, -127, 128)));
        }
      }
    }
    else
    {
      Log_ErrorPrintf("Malformed line: '%s'", line.c_str());
      return false;
    }
  }

  if (m_input_mappings.empty())
  {
    Log_ErrorPrintf("Missing input mapping");
    return false;
  }

  m_num_frames = static_cast<u32>(m_input_values.size() / m_input_mappings.size());
  if (m_num_frames == 0)
  {
    Log_ErrorPrintf("Missing input data");
    return false;
  }

  Log_InfoPrintf("Mapped %zu inputs", m_input_mappings.size());
  Log_InfoPrintf("Loaded inputs for %u frames", m_num_frames);
  return true;
}
