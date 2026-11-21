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

TEST(MacroSequence, ConvertRecorded)
{
  // Waits start from the first input, and changes to a bind within a frame are merged.
  const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(ControllerType::DigitalController);
  const u32 cross = GetBindIndex(cinfo, "Cross");
  const u32 square = GetBindIndex(cinfo, "Square");
  const MacroSequenceAction recorded[] = {
    {cross, 10, 1.0f}, {square, 10, 0.5f}, {square, 10, 1.0f}, {cross, 14, 0.0f}, {square, 20, 0.0f},
  };
  EXPECT_EQ(InputManager::FormatMacroSequence(InputManager::ConvertRecordedMacroActions(recorded, false), cinfo),
            "Cross Square 4 Cross:0 6 Square:0");
  EXPECT_EQ(InputManager::FormatMacroSequence(InputManager::ConvertRecordedMacroActions(recorded, true), cinfo),
            "Cross Square 4 Cross:0 6 Square:0");
  EXPECT_TRUE(InputManager::ConvertRecordedMacroActions({}, true).empty());
}

TEST(MacroSequence, ConvertRecordedSameFrameRelease)
{
  // A press and release in the same frame is held for a frame, so the game still sees it.
  const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(ControllerType::DigitalController);
  const u32 cross = GetBindIndex(cinfo, "Cross");
  const u32 square = GetBindIndex(cinfo, "Square");
  const MacroSequenceAction recorded[] = {
    {cross, 0, 1.0f}, {cross, 0, 0.0f}, {square, 3, 1.0f}, {square, 3, 0.0f}, {cross, 5, 1.0f},
  };
  EXPECT_EQ(InputManager::FormatMacroSequence(InputManager::ConvertRecordedMacroActions(recorded, false), cinfo),
            "Cross 1 Cross:0 2 Square 1 Square:0 1 Cross");
}

TEST(MacroSequence, ConvertRecordedReleaseHeld)
{
  // Binds still held at the end are released a frame later.
  const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(ControllerType::DigitalController);
  const u32 cross = GetBindIndex(cinfo, "Cross");
  const u32 square = GetBindIndex(cinfo, "Square");
  const MacroSequenceAction recorded[] = {{cross, 2, 1.0f}, {square, 4, 1.0f}, {cross, 6, 0.0f}};
  EXPECT_EQ(InputManager::FormatMacroSequence(InputManager::ConvertRecordedMacroActions(recorded, false), cinfo),
            "Cross 2 Square 2 Cross:0");
  EXPECT_EQ(InputManager::FormatMacroSequence(InputManager::ConvertRecordedMacroActions(recorded, true), cinfo),
            "Cross 2 Square 2 Cross:0 1 Square:0");
}
