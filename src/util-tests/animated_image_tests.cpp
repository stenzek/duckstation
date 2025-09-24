// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/animated_image.h"

#include "common/error.h"

#include <gtest/gtest.h>
#include <vector>

namespace {
// Test fixture for AnimatedImage tests
class AnimatedImageTest : public ::testing::Test
{
protected:
  // Helper method to create test images with a pattern
  AnimatedImage CreateTestImage(u32 width, u32 height, u32 frames = 1)
  {
    AnimatedImage img(width, height, frames, {1, 10});
    for (u32 f = 0; f < frames; f++)
    {
      for (u32 y = 0; y < height; y++)
      {
        for (u32 x = 0; x < width; x++)
        {
          img.GetPixels(f)[y * width + x] = (x + y + f) | 0xFF000000; // Make pixel opaque
        }
      }
    }
    return img;
  }
};

} // namespace

// Constructor Tests
TEST_F(AnimatedImageTest, DefaultConstructor)
{
  AnimatedImage img;
  EXPECT_FALSE(img.IsValid());
  EXPECT_EQ(img.GetWidth(), 0u);
  EXPECT_EQ(img.GetHeight(), 0u);
  EXPECT_EQ(img.GetFrames(), 0u);
}

TEST_F(AnimatedImageTest, ParameterizedConstructor)
{
  const u32 width = 100;
  const u32 height = 80;
  const u32 frames = 5;
  AnimatedImage::FrameDelay delay{1, 10};

  AnimatedImage img(width, height, frames, delay);
  EXPECT_TRUE(img.IsValid());
  EXPECT_EQ(img.GetWidth(), width);
  EXPECT_EQ(img.GetHeight(), height);
  EXPECT_EQ(img.GetFrames(), frames);
  EXPECT_EQ(img.GetFrameSize(), width * height);

  // Check all frames have the same delay
  for (u32 i = 0; i < frames; i++)
  {
    EXPECT_EQ(img.GetFrameDelay(i).numerator, delay.numerator);
    EXPECT_EQ(img.GetFrameDelay(i).denominator, delay.denominator);
  }
}

TEST_F(AnimatedImageTest, CopyConstructor)
{
  auto original = CreateTestImage(50, 40, 2);
  AnimatedImage copy(original);

  EXPECT_EQ(copy.GetWidth(), original.GetWidth());
  EXPECT_EQ(copy.GetHeight(), original.GetHeight());
  EXPECT_EQ(copy.GetFrames(), original.GetFrames());

  // Check pixels match
  for (u32 f = 0; f < original.GetFrames(); f++)
  {
    for (u32 i = 0; i < original.GetFrameSize(); i++)
    {
      EXPECT_EQ(copy.GetPixels(f)[i], original.GetPixels(f)[i]);
    }
  }
}

TEST_F(AnimatedImageTest, MoveConstructor)
{
  auto original = CreateTestImage(50, 40, 2);
  const u32 width = original.GetWidth();
  const u32 height = original.GetHeight();
  const u32 frames = original.GetFrames();

  // Store original pixel data to compare later
  std::vector<std::vector<u32>> pixel_copies;
  for (u32 f = 0; f < frames; f++)
  {
    pixel_copies.push_back(std::vector<u32>(original.GetPixels(f), original.GetPixels(f) + original.GetFrameSize()));
  }

  AnimatedImage moved(std::move(original));

  EXPECT_FALSE(original.IsValid()); // Original should be invalid after move
  EXPECT_EQ(moved.GetWidth(), width);
  EXPECT_EQ(moved.GetHeight(), height);
  EXPECT_EQ(moved.GetFrames(), frames);

  // Check pixels were moved correctly
  for (u32 f = 0; f < frames; f++)
  {
    for (u32 i = 0; i < width * height; i++)
    {
      EXPECT_EQ(moved.GetPixels(f)[i], pixel_copies[f][i]);
    }
  }
}

// Assignment Operator Tests
TEST_F(AnimatedImageTest, CopyAssignment)
{
  auto original = CreateTestImage(60, 50, 3);
  AnimatedImage copy;
  copy = original;

  EXPECT_EQ(copy.GetWidth(), original.GetWidth());
  EXPECT_EQ(copy.GetHeight(), original.GetHeight());
  EXPECT_EQ(copy.GetFrames(), original.GetFrames());

  // Check pixels match
  for (u32 f = 0; f < original.GetFrames(); f++)
  {
    for (u32 i = 0; i < original.GetFrameSize(); i++)
    {
      EXPECT_EQ(copy.GetPixels(f)[i], original.GetPixels(f)[i]);
    }
  }
}

TEST_F(AnimatedImageTest, MoveAssignment)
{
  auto original = CreateTestImage(60, 50, 3);
  const u32 width = original.GetWidth();
  const u32 height = original.GetHeight();
  const u32 frames = original.GetFrames();

  std::vector<std::vector<u32>> pixel_copies;
  for (u32 f = 0; f < frames; f++)
  {
    pixel_copies.push_back(std::vector<u32>(original.GetPixels(f), original.GetPixels(f) + original.GetFrameSize()));
  }

  AnimatedImage moved;
  moved = std::move(original);

  EXPECT_FALSE(original.IsValid()); // Original should be invalid after move
  EXPECT_EQ(moved.GetWidth(), width);
  EXPECT_EQ(moved.GetHeight(), height);
  EXPECT_EQ(moved.GetFrames(), frames);

  // Check pixels were moved correctly
  for (u32 f = 0; f < frames; f++)
  {
    for (u32 i = 0; i < width * height; i++)
    {
      EXPECT_EQ(moved.GetPixels(f)[i], pixel_copies[f][i]);
    }
  }
}

// Pixel Access Tests
TEST_F(AnimatedImageTest, PixelAccess)
{
  const u32 width = 10;
  const u32 height = 8;
  AnimatedImage img(width, height, 1, {1, 10});

  // Test direct pixel access
  for (u32 y = 0; y < height; y++)
  {
    for (u32 x = 0; x < width; x++)
    {
      img.GetPixels(0)[y * width + x] = 0xFF000000u | (x + y * width);
    }
  }

  // Verify pixels
  for (u32 y = 0; y < height; y++)
  {
    for (u32 x = 0; x < width; x++)
    {
      EXPECT_EQ(img.GetPixels(0)[y * width + x], 0xFF000000u | (x + y * width));
      EXPECT_EQ(img.GetRowPixels(0, y)[x], 0xFF000000u | (x + y * width));
    }
  }

  // Test GetPixelsSpan
  auto span = img.GetPixelsSpan(0);
  for (u32 i = 0; i < width * height; i++)
  {
    EXPECT_EQ(span[i], 0xFF000000u | i);
  }
}

TEST_F(AnimatedImageTest, SetPixels)
{
  const u32 width = 10;
  const u32 height = 8;
  AnimatedImage img(width, height, 1, {1, 10});

  // Create source pixels
  std::vector<u32> src_pixels(width * height);
  for (u32 i = 0; i < width * height; i++)
  {
    src_pixels[i] = 0xFF000000 | i;
  }

  // Copy with SetPixels
  img.SetPixels(0, src_pixels.data(), width * sizeof(u32));

  // Verify pixels
  for (u32 i = 0; i < width * height; i++)
  {
    EXPECT_EQ(img.GetPixels(0)[i], 0xFF000000u | i);
  }
}

TEST_F(AnimatedImageTest, SetDelay)
{
  AnimatedImage img(10, 10, 2, {1, 10});

  AnimatedImage::FrameDelay delay{2, 20};
  img.SetDelay(1, delay);

  EXPECT_EQ(img.GetFrameDelay(1).numerator, 2);
  EXPECT_EQ(img.GetFrameDelay(1).denominator, 20);

  // First frame should be unchanged
  EXPECT_EQ(img.GetFrameDelay(0).numerator, 1);
  EXPECT_EQ(img.GetFrameDelay(0).denominator, 10);
}

// Image Manipulation Tests
TEST_F(AnimatedImageTest, Resize)
{
  AnimatedImage img = CreateTestImage(10, 8, 2);
  AnimatedImage img2 = CreateTestImage(10, 8, 2);

  // Resize to larger dimensions, preserving content
  img.Resize(20, 16, 3, {1, 10}, true);

  EXPECT_EQ(img.GetWidth(), 20u);
  EXPECT_EQ(img.GetHeight(), 16u);
  EXPECT_EQ(img.GetFrames(), 3u);

  // Check that original content is preserved
  for (u32 f = 0; f < 2; f++)
  {
    for (u32 y = 0; y < 8; y++)
    {
      for (u32 x = 0; x < 10; x++)
      {
        EXPECT_EQ(img.GetRowPixels(f, y)[x] & 0xFFu, (x + y + f) & 0xFFu);
      }
    }
  }

  // Check that new areas are zeroed
  for (u32 f = 0; f < 2; f++)
  {
    for (u32 y = 0; y < 8; y++)
    {
      for (u32 x = 10; x < 20; x++)
      {
        EXPECT_EQ(img.GetRowPixels(f, y)[x], 0u);
      }
    }
    for (u32 y = 8; y < 16; y++)
    {
      for (u32 x = 0; x < 20; x++)
      {
        EXPECT_EQ(img.GetRowPixels(f, y)[x], 0u);
      }
    }
  }

  // Check third frame has the specified default delay
  EXPECT_EQ(img.GetFrameDelay(2).numerator, 1u);
  EXPECT_EQ(img.GetFrameDelay(2).denominator, 10u);
}

TEST_F(AnimatedImageTest, Clear)
{
  AnimatedImage img = CreateTestImage(10, 8, 2);

  img.Clear();

  // Dimensions should remain the same
  EXPECT_EQ(img.GetWidth(), 10u);
  EXPECT_EQ(img.GetHeight(), 8u);
  EXPECT_EQ(img.GetFrames(), 2u);

  // All pixels should be zeroed
  for (u32 f = 0; f < img.GetFrames(); f++)
  {
    for (u32 i = 0; i < img.GetFrameSize(); i++)
    {
      EXPECT_EQ(img.GetPixels(f)[i], 0u);
    }
  }
}

TEST_F(AnimatedImageTest, Invalidate)
{
  AnimatedImage img = CreateTestImage(10, 8, 2);

  img.Invalidate();

  EXPECT_FALSE(img.IsValid());
  EXPECT_EQ(img.GetWidth(), 0u);
  EXPECT_EQ(img.GetHeight(), 0u);
  EXPECT_EQ(img.GetFrames(), 0u);
}

TEST_F(AnimatedImageTest, TakePixels)
{
  AnimatedImage img = CreateTestImage(10, 8, 2);
  const u32 expected_size = 10 * 8 * 2;

  auto pixels = img.TakePixels();

  // Image should be invalidated
  EXPECT_FALSE(img.IsValid());
  EXPECT_EQ(img.GetWidth(), 0u);
  EXPECT_EQ(img.GetHeight(), 0u);
  EXPECT_EQ(img.GetFrames(), 0u);

  // Pixel storage should have the expected size
  EXPECT_EQ(pixels.size(), expected_size);
}

// File Operations Tests
TEST_F(AnimatedImageTest, LoadSavePNG)
{
  // Create test image
  AnimatedImage original = CreateTestImage(20, 16, 1);

  // Set specific pixel patterns for verification
  original.GetPixels(0)[0] = 0xFF0000FFu; // Blue
  original.GetPixels(0)[1] = 0xFF00FF00u; // Green
  original.GetPixels(0)[2] = 0xFFFF0000u; // Red

  // Save to file
  auto buffer = original.SaveToBuffer("test_image.png");
  ASSERT_TRUE(buffer.has_value());

  // Load the image back
  AnimatedImage loaded;
  ASSERT_TRUE(loaded.LoadFromBuffer("test_image.png", buffer.value()));

  // Compare dimensions
  EXPECT_EQ(loaded.GetWidth(), original.GetWidth());
  EXPECT_EQ(loaded.GetHeight(), original.GetHeight());
  EXPECT_EQ(loaded.GetFrames(), 1u);

  // Compare specific pixel colors (ignoring alpha variations)
  EXPECT_EQ(loaded.GetPixels(0)[0] & 0xFFFFFFu, 0x0000FFu); // Blue
  EXPECT_EQ(loaded.GetPixels(0)[1] & 0xFFFFFFu, 0x00FF00u); // Green
  EXPECT_EQ(loaded.GetPixels(0)[2] & 0xFFFFFFu, 0xFF0000u); // Red
}

TEST_F(AnimatedImageTest, LoadSaveMultiFramePNG)
{
  // Create multi-frame test image
  AnimatedImage original = CreateTestImage(20, 16, 2);

  // Set different delays for frames
  original.SetDelay(0, {1, 10});
  original.SetDelay(1, {2, 20});

  // Save to file
  auto buffer = original.SaveToBuffer("test_anim.png");
  ASSERT_TRUE(buffer.has_value());

  // Load back
  AnimatedImage loaded;
  ASSERT_TRUE(loaded.LoadFromBuffer("test_anim.png", buffer.value()));

  // Compare dimensions and frame count
  EXPECT_EQ(loaded.GetWidth(), original.GetWidth());
  EXPECT_EQ(loaded.GetHeight(), original.GetHeight());
  EXPECT_EQ(loaded.GetFrames(), original.GetFrames());

  // Compare frame delays
  EXPECT_EQ(loaded.GetFrameDelay(0).numerator, 1u);
  EXPECT_EQ(loaded.GetFrameDelay(0).denominator, 10u);
  EXPECT_EQ(loaded.GetFrameDelay(1).numerator, 2u);
  EXPECT_EQ(loaded.GetFrameDelay(1).denominator, 20u);
}

TEST_F(AnimatedImageTest, SaveLoadBuffer)
{
  AnimatedImage original = CreateTestImage(20, 16, 1);

  // Save to buffer
  auto buffer = original.SaveToBuffer("test.png");
  ASSERT_TRUE(buffer.has_value());
  EXPECT_GT(buffer->size(), 0u);

  // Load from buffer
  AnimatedImage loaded;
  ASSERT_TRUE(loaded.LoadFromBuffer("test.png", *buffer));

  // Compare dimensions
  EXPECT_EQ(loaded.GetWidth(), original.GetWidth());
  EXPECT_EQ(loaded.GetHeight(), original.GetHeight());

  // Compare some pixels (ignoring alpha)
  for (u32 i = 0; i < std::min(10u, original.GetFrameSize()); i++)
  {
    EXPECT_EQ(loaded.GetPixels(0)[i] & 0xFFFFFF, original.GetPixels(0)[i] & 0xFFFFFF);
  }
}

TEST_F(AnimatedImageTest, ErrorHandling)
{
  AnimatedImage img;
  Error err;

  // Try loading non-existent file
  EXPECT_FALSE(img.LoadFromFile("non_existent_file.png", &err));
  EXPECT_TRUE(err.IsValid());

  // Try loading file with invalid extension
  EXPECT_FALSE(img.LoadFromFile("test.invalid", &err));
  EXPECT_TRUE(err.IsValid());
}

TEST_F(AnimatedImageTest, CalculatePitch)
{
  EXPECT_EQ(AnimatedImage::CalculatePitch(10, 5), 10 * sizeof(u32));
  EXPECT_EQ(AnimatedImage::CalculatePitch(100, 200), 100 * sizeof(u32));
}

// Multiple frame handling and frame delay tests
TEST_F(AnimatedImageTest, MultipleFrameDelays)
{
  const u32 width = 32;
  const u32 height = 24;
  const u32 frames = 5;
  AnimatedImage img(width, height, frames, {1, 10});

  // Set different delays for each frame
  img.SetDelay(0, {1, 10});
  img.SetDelay(1, {2, 20});
  img.SetDelay(2, {3, 30});
  img.SetDelay(3, {4, 40});
  img.SetDelay(4, {5, 50});

  // Verify each frame has the correct delay
  EXPECT_EQ(img.GetFrameDelay(0).numerator, 1u);
  EXPECT_EQ(img.GetFrameDelay(0).denominator, 10u);
  EXPECT_EQ(img.GetFrameDelay(1).numerator, 2u);
  EXPECT_EQ(img.GetFrameDelay(1).denominator, 20u);
  EXPECT_EQ(img.GetFrameDelay(2).numerator, 3u);
  EXPECT_EQ(img.GetFrameDelay(2).denominator, 30u);
  EXPECT_EQ(img.GetFrameDelay(3).numerator, 4u);
  EXPECT_EQ(img.GetFrameDelay(3).denominator, 40u);
  EXPECT_EQ(img.GetFrameDelay(4).numerator, 5u);
  EXPECT_EQ(img.GetFrameDelay(4).denominator, 50u);
}

TEST_F(AnimatedImageTest, PreserveFrameDelaysOnResize)
{
  const u32 width = 16;
  const u32 height = 16;
  const u32 frames = 3;

  AnimatedImage img(width, height, frames, {1, 10});

  // Set unique delays for each frame
  img.SetDelay(0, {5, 25});
  img.SetDelay(1, {10, 50});
  img.SetDelay(2, {15, 75});

  // Resize with fewer frames - should preserve existing frame delays
  img.Resize(32, 32, 2, {20, 100}, true);

  EXPECT_EQ(img.GetWidth(), 32u);
  EXPECT_EQ(img.GetHeight(), 32u);
  EXPECT_EQ(img.GetFrames(), 2u);

  // Original frame delays should be preserved
  EXPECT_EQ(img.GetFrameDelay(0).numerator, 5u);
  EXPECT_EQ(img.GetFrameDelay(0).denominator, 25u);
  EXPECT_EQ(img.GetFrameDelay(1).numerator, 10u);
  EXPECT_EQ(img.GetFrameDelay(1).denominator, 50u);

  // Resize with more frames - new frames should use the default delay
  img.Resize(32, 32, 4, {20, 100}, true);

  EXPECT_EQ(img.GetFrames(), 4u);

  // Original frame delays should still be preserved
  EXPECT_EQ(img.GetFrameDelay(0).numerator, 5u);
  EXPECT_EQ(img.GetFrameDelay(0).denominator, 25u);
  EXPECT_EQ(img.GetFrameDelay(1).numerator, 10u);
  EXPECT_EQ(img.GetFrameDelay(1).denominator, 50u);

  // New frames should have the default delay
  EXPECT_EQ(img.GetFrameDelay(2).numerator, 20u);
  EXPECT_EQ(img.GetFrameDelay(2).denominator, 100u);
  EXPECT_EQ(img.GetFrameDelay(3).numerator, 20u);
  EXPECT_EQ(img.GetFrameDelay(3).denominator, 100u);
}

TEST_F(AnimatedImageTest, IndividualFrameModification)
{
  const u32 width = 8;
  const u32 height = 8;
  const u32 frames = 3;

  AnimatedImage img(width, height, frames, {1, 10});

  // Set distinct patterns for each frame
  for (u32 f = 0; f < frames; f++)
  {
    for (u32 y = 0; y < height; y++)
    {
      for (u32 x = 0; x < width; x++)
      {
        // Frame 0: all red, Frame 1: all green, Frame 2: all blue
        u32 color = (f == 0) ? 0xFF0000FFu : ((f == 1) ? 0xFF00FF00u : 0xFFFF0000u);
        img.GetPixels(f)[y * width + x] = color;
      }
    }
  }

  // Verify each frame has the correct pattern
  for (u32 y = 0; y < height; y++)
  {
    for (u32 x = 0; x < width; x++)
    {
      EXPECT_EQ(img.GetPixels(0)[y * width + x], 0xFF0000FFu); // Red
      EXPECT_EQ(img.GetPixels(1)[y * width + x], 0xFF00FF00u); // Green
      EXPECT_EQ(img.GetPixels(2)[y * width + x], 0xFFFF0000u); // Blue
    }
  }

  // Modify only the middle frame
  for (u32 y = 0; y < height; y++)
  {
    for (u32 x = 0; x < width; x++)
    {
      img.GetPixels(1)[y * width + x] = 0xFFFFFFFFu; // White
    }
  }

  // Verify only the middle frame was changed
  for (u32 y = 0; y < height; y++)
  {
    for (u32 x = 0; x < width; x++)
    {
      EXPECT_EQ(img.GetPixels(0)[y * width + x], 0xFF0000FFu); // Still red
      EXPECT_EQ(img.GetPixels(1)[y * width + x], 0xFFFFFFFFu); // Now white
      EXPECT_EQ(img.GetPixels(2)[y * width + x], 0xFFFF0000u); // Still blue
    }
  }
}

TEST_F(AnimatedImageTest, MultiFrameAnimationRoundTrip)
{
  const u32 width = 24;
  const u32 height = 24;
  const u32 frames = 4;

  // Create a test animation with 4 frames, each with different content and timing
  AnimatedImage original(width, height, frames, {1, 10});

  // Frame 0: Red with delay 1/10
  std::memset(original.GetPixels(0), 0, width * height * sizeof(u32));
  for (u32 i = 0; i < width * height; i++)
  {
    original.GetPixels(0)[i] = 0xFF0000FFu; // Red
  }
  original.SetDelay(0, {1, 10});

  // Frame 1: Green with delay 2/20
  std::memset(original.GetPixels(1), 0, width * height * sizeof(u32));
  for (u32 i = 0; i < width * height; i++)
  {
    original.GetPixels(1)[i] = 0xFF00FF00u; // Green
  }
  original.SetDelay(1, {2, 20});

  // Frame 2: Blue with delay 3/30
  std::memset(original.GetPixels(2), 0, width * height * sizeof(u32));
  for (u32 i = 0; i < width * height; i++)
  {
    original.GetPixels(2)[i] = 0xFFFF0000u; // Blue
  }
  original.SetDelay(2, {3, 30});

  // Frame 3: Yellow with delay 4/40
  std::memset(original.GetPixels(3), 0, width * height * sizeof(u32));
  for (u32 i = 0; i < width * height; i++)
  {
    original.GetPixels(3)[i] = 0xFF00FFFFu; // Yellow
  }
  original.SetDelay(3, {4, 40});

  // Save to buffer
  auto buffer = original.SaveToBuffer("test_animation.png");
  ASSERT_TRUE(buffer.has_value());

  // Load back from buffer
  AnimatedImage loaded;
  ASSERT_TRUE(loaded.LoadFromBuffer("test_animation.png", *buffer));

  // Verify dimensions and frame count
  EXPECT_EQ(loaded.GetWidth(), width);
  EXPECT_EQ(loaded.GetHeight(), height);
  EXPECT_EQ(loaded.GetFrames(), frames);

  // Verify frame delays
  EXPECT_EQ(loaded.GetFrameDelay(0).numerator, 1u);
  EXPECT_EQ(loaded.GetFrameDelay(0).denominator, 10u);
  EXPECT_EQ(loaded.GetFrameDelay(1).numerator, 2u);
  EXPECT_EQ(loaded.GetFrameDelay(1).denominator, 20u);
  EXPECT_EQ(loaded.GetFrameDelay(2).numerator, 3u);
  EXPECT_EQ(loaded.GetFrameDelay(2).denominator, 30u);
  EXPECT_EQ(loaded.GetFrameDelay(3).numerator, 4u);
  EXPECT_EQ(loaded.GetFrameDelay(3).denominator, 40u);

  // Verify frame contents (sampling first pixel of each frame)
  EXPECT_EQ(loaded.GetPixels(0)[0] & 0xFFFFFFu, 0x0000FFu); // Red
  EXPECT_EQ(loaded.GetPixels(1)[0] & 0xFFFFFFu, 0x00FF00u); // Green
  EXPECT_EQ(loaded.GetPixels(2)[0] & 0xFFFFFFu, 0xFF0000u); // Blue
  EXPECT_EQ(loaded.GetPixels(3)[0] & 0xFFFFFFu, 0x00FFFFu); // Yellow
}

TEST_F(AnimatedImageTest, MaximumAndZeroDelays)
{
  const u32 width = 16;
  const u32 height = 16;
  const u32 frames = 4;

  AnimatedImage img(width, height, frames, {1, 10});

  // Set extreme delay values
  img.SetDelay(0, {0, 1});     // Zero numerator (minimum)
  img.SetDelay(1, {65535, 1}); // Maximum numerator (u16 max)
  img.SetDelay(2, {1, 65535}); // Maximum denominator (u16 max)
  img.SetDelay(3, {0, 65535}); // Both extreme

  // Verify delay values were set correctly
  EXPECT_EQ(img.GetFrameDelay(0).numerator, 0u);
  EXPECT_EQ(img.GetFrameDelay(0).denominator, 1u);
  EXPECT_EQ(img.GetFrameDelay(1).numerator, 65535u);
  EXPECT_EQ(img.GetFrameDelay(1).denominator, 1u);
  EXPECT_EQ(img.GetFrameDelay(2).numerator, 1u);
  EXPECT_EQ(img.GetFrameDelay(2).denominator, 65535u);
  EXPECT_EQ(img.GetFrameDelay(3).numerator, 0u);
  EXPECT_EQ(img.GetFrameDelay(3).denominator, 65535u);

  // Save to buffer and load back to verify these values are preserved
  auto buffer = img.SaveToBuffer("test_delays.png");
  ASSERT_TRUE(buffer.has_value());

  AnimatedImage loaded;
  ASSERT_TRUE(loaded.LoadFromBuffer("test_delays.png", *buffer));

  // Verify delays are preserved
  EXPECT_EQ(loaded.GetFrameDelay(0).numerator, 0u);
  EXPECT_EQ(loaded.GetFrameDelay(0).denominator, 1u);
  EXPECT_EQ(loaded.GetFrameDelay(1).numerator, 65535u);
  EXPECT_EQ(loaded.GetFrameDelay(1).denominator, 1u);
  EXPECT_EQ(loaded.GetFrameDelay(2).numerator, 1u);
  EXPECT_EQ(loaded.GetFrameDelay(2).denominator, 65535u);
  EXPECT_EQ(loaded.GetFrameDelay(3).numerator, 0u);
  EXPECT_EQ(loaded.GetFrameDelay(3).denominator, 65535u);
}

TEST_F(AnimatedImageTest, ResizeBetweenSingleAndMultipleFrames)
{
  // Start with a single frame
  AnimatedImage img(16, 16, 1, {1, 10});
  EXPECT_EQ(img.GetFrames(), 1u);

  // Fill with a pattern
  for (u32 i = 0; i < img.GetFrameSize(); i++)
  {
    img.GetPixels(0)[i] = 0xFF000000u | i;
  }

  // Resize to multiple frames
  img.Resize(16, 16, 3, {2, 20}, true);
  EXPECT_EQ(img.GetFrames(), 3u);

  // Verify original frame is preserved
  for (u32 i = 0; i < img.GetFrameSize(); i++)
  {
    EXPECT_EQ(img.GetPixels(0)[i], 0xFF000000u | i);
  }

  // Fill second frame with different pattern
  for (u32 i = 0; i < img.GetFrameSize(); i++)
  {
    img.GetPixels(1)[i] = 0xFF000000u | (i * 2);
  }

  // Resize back to single frame
  img.Resize(16, 16, 1, {3, 30}, true);
  EXPECT_EQ(img.GetFrames(), 1u);

  // Verify first frame is still preserved
  for (u32 i = 0; i < img.GetFrameSize(); i++)
  {
    EXPECT_EQ(img.GetPixels(0)[i], 0xFF000000u | i);
  }
}
