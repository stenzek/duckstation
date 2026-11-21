// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/controller.h"

#include "util/input_manager.h"

#include <gtest/gtest.h>

using InputManager::MacroSequenceAction;

static u32 GetBindIndex(const Controller::ControllerInfo& cinfo, std::string_view name)
{
  for (const Controller::ControllerBindingInfo& bi : cinfo.bindings)
  {
    if (name == bi.name)
      return bi.bind_index;
  }

  ADD_FAILURE() << "Unknown bind " << name;
  return 0;
}

TEST(MacroSequence, ParseAndFormat)
{
  const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(ControllerType::DigitalController);
  const std::vector<MacroSequenceAction> actions =
    InputManager::ParseMacroSequence("Cross 4 Cross:0 Square:0.5  Up 10", cinfo);
  ASSERT_EQ(actions.size(), 6u);
  EXPECT_EQ(actions[0].bind_index, GetBindIndex(cinfo, "Cross"));
  EXPECT_EQ(actions[0].value, 1.0f);
  EXPECT_EQ(actions[1].bind_index, MacroSequenceAction::WAIT_BIND_INDEX);
  EXPECT_EQ(actions[1].frames, 4u);
  EXPECT_EQ(actions[2].bind_index, GetBindIndex(cinfo, "Cross"));
  EXPECT_EQ(actions[2].value, 0.0f);
  EXPECT_EQ(actions[3].bind_index, GetBindIndex(cinfo, "Square"));
  EXPECT_EQ(actions[3].value, 0.5f);
  EXPECT_EQ(actions[4].bind_index, GetBindIndex(cinfo, "Up"));
  EXPECT_EQ(actions[5].frames, 10u);

  EXPECT_EQ(InputManager::FormatMacroSequence(actions, cinfo), "Cross 4 Cross:0 Square:0.5 Up 10");
}

TEST(MacroSequence, ParseSkipsInvalidTokens)
{
  const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(ControllerType::DigitalController);

  // Unknown binds, bad values, trailing junk on waits and zero-frame waits are dropped. Values are clamped.
  const std::vector<MacroSequenceAction> actions =
    InputManager::ParseMacroSequence("Bogus Cross:abc 4x 0 Circle:2 Triangle:-1", cinfo);
  ASSERT_EQ(actions.size(), 2u);
  EXPECT_EQ(actions[0].bind_index, GetBindIndex(cinfo, "Circle"));
  EXPECT_EQ(actions[0].value, 1.0f);
  EXPECT_EQ(actions[1].bind_index, GetBindIndex(cinfo, "Triangle"));
  EXPECT_EQ(actions[1].value, 0.0f);

  EXPECT_TRUE(InputManager::ParseMacroSequence("", cinfo).empty());
}

TEST(MacroSequence, FormatRoundsValues)
{
  const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(ControllerType::AnalogController);
  const MacroSequenceAction actions[] = {
    {GetBindIndex(cinfo, "LLeft"), 0, 1.0f / 3.0f},
    {MacroSequenceAction::WAIT_BIND_INDEX, 2, 0.0f},
    {GetBindIndex(cinfo, "LLeft"), 0, 0.0f},
  };
  EXPECT_EQ(InputManager::FormatMacroSequence(actions, cinfo), "LLeft:0.333 2 LLeft:0");
}

TEST(MacroSequence, ConvertLegacyChord)
{
  // A chord without a toggle frequency just holds the binds.
  const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(ControllerType::DigitalController);
  const std::vector<MacroSequenceAction> actions =
    InputManager::ConvertLegacyMacroToSequence("Cross&Square", 0.5f, 0, cinfo);
  EXPECT_EQ(InputManager::FormatMacroSequence(actions, cinfo), "Cross:0.5 Square:0.5");
}

TEST(MacroSequence, ConvertLegacyTurbo)
{
  // Turbo is on for the frequency, then off for the frequency.
  const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(ControllerType::DigitalController);
  const std::vector<MacroSequenceAction> actions =
    InputManager::ConvertLegacyMacroToSequence("Cross&Bogus&Square", 1.0f, 3, cinfo);
  EXPECT_EQ(InputManager::FormatMacroSequence(actions, cinfo), "Cross Square 3 Cross:0 Square:0 3");

  EXPECT_TRUE(InputManager::ConvertLegacyMacroToSequence("Bogus", 1.0f, 3, cinfo).empty());
}
