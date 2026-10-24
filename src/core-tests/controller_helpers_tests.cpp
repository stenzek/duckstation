// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/controller.h"
#include "core/controller_helpers.h"

#include <gtest/gtest.h>

TEST(ControllerHelpers, MergeHalfAxesCenter)
{
  // Equal opposing inputs must produce center, not 0x80 (breaks Persona 2 up/down).
  for (u32 i = 0; i <= 255; i++)
  {
    EXPECT_EQ(ControllerHelpers::MergeHalfAxes(static_cast<u8>(i), static_cast<u8>(i), false), Controller::AXIS_CENTER);
    EXPECT_EQ(ControllerHelpers::MergeHalfAxes(static_cast<u8>(i), static_cast<u8>(i), true), Controller::AXIS_CENTER);
  }
}

TEST(ControllerHelpers, MergeHalfAxesRange)
{
  EXPECT_EQ(ControllerHelpers::MergeHalfAxes(255, 0, false), 0);
  EXPECT_EQ(ControllerHelpers::MergeHalfAxes(0, 255, false), 255);
  EXPECT_EQ(ControllerHelpers::MergeHalfAxes(255, 0, true), 255);
  EXPECT_EQ(ControllerHelpers::MergeHalfAxes(0, 255, true), 0);

  // Only the net deflection matters when both sides are held.
  EXPECT_EQ(ControllerHelpers::MergeHalfAxes(100, 255, false), ControllerHelpers::MergeHalfAxes(0, 155, false));
  EXPECT_EQ(ControllerHelpers::MergeHalfAxes(255, 100, false), ControllerHelpers::MergeHalfAxes(155, 0, false));
}
