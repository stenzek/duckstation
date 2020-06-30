#pragma once
#include "types.h"
#include <array>

namespace Resources {

// Adapted from https://icons8.com/icon/15970/target
constexpr u32 CROSSHAIR_IMAGE_WIDTH = 96;
constexpr u32 CROSSHAIR_IMAGE_HEIGHT = 96;
extern const std::array<u32, CROSSHAIR_IMAGE_WIDTH * CROSSHAIR_IMAGE_HEIGHT> CROSSHAIR_IMAGE_DATA;

} // namespace Resources
