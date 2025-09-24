// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/image.h"

#include "common/error.h"

#include "gtest/gtest.h"

#include <type_traits>

namespace {

class ImageTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    // Default test image is a 4x4 RGBA8 image
    m_test_image = Image(4, 4, ImageFormat::RGBA8);
    // Fill with a simple pattern (red gradient)
    for (u32 y = 0; y < m_test_image.GetHeight(); y++)
    {
      for (u32 x = 0; x < m_test_image.GetWidth(); x++)
      {
        u32* pixel = reinterpret_cast<u32*>(m_test_image.GetRowPixels(y) + x * sizeof(u32));
        // Red gradient, full alpha
        *pixel = (x * 64) | (0u << 8) | (0u << 16) | (0xFFu << 24);
      }
    }
  }

  Image m_test_image;
};

} // namespace

// Basic constructor tests
TEST_F(ImageTest, DefaultConstructor)
{
  Image img;
  EXPECT_FALSE(img.IsValid());
  EXPECT_EQ(img.GetWidth(), 0u);
  EXPECT_EQ(img.GetHeight(), 0u);
  EXPECT_EQ(img.GetFormat(), ImageFormat::None);
}

TEST_F(ImageTest, SizeFormatConstructor)
{
  const u32 width = 16;
  const u32 height = 8;
  Image img(width, height, ImageFormat::RGBA8);

  EXPECT_TRUE(img.IsValid());
  EXPECT_EQ(img.GetWidth(), width);
  EXPECT_EQ(img.GetHeight(), height);
  EXPECT_EQ(img.GetFormat(), ImageFormat::RGBA8);
  EXPECT_NE(img.GetPixels(), nullptr);
}

TEST_F(ImageTest, CopyConstructor)
{
  const u32 width = 16;
  const u32 height = 8;
  Image src(width, height, ImageFormat::RGBA8);

  // Set a test pattern
  std::memset(src.GetPixels(), 0xAA, src.GetStorageSize());

  // Copy construct
  Image copy(src);

  EXPECT_TRUE(copy.IsValid());
  EXPECT_EQ(copy.GetWidth(), width);
  EXPECT_EQ(copy.GetHeight(), height);
  EXPECT_EQ(copy.GetFormat(), ImageFormat::RGBA8);

  // Contents should be the same
  EXPECT_EQ(std::memcmp(copy.GetPixels(), src.GetPixels(), src.GetStorageSize()), 0);
  // But the memory should be different
  EXPECT_NE(copy.GetPixels(), src.GetPixels());
}

// Assignment operator tests
TEST_F(ImageTest, CopyAssignmentOperator)
{
  const u32 width = 8;
  const u32 height = 6;
  Image src(width, height, ImageFormat::RGBA8);

  // Set a test pattern
  std::memset(src.GetPixels(), 0xCCu, src.GetStorageSize());

  // Create a different image first
  Image dest(4, 4, ImageFormat::BGRA8);
  std::memset(dest.GetPixels(), 0x55u, dest.GetStorageSize());

  // Assign using copy assignment
  dest = src;

  // Verify properties were copied
  EXPECT_EQ(dest.GetWidth(), width);
  EXPECT_EQ(dest.GetHeight(), height);
  EXPECT_EQ(dest.GetFormat(), ImageFormat::RGBA8);

  // Contents should be the same
  EXPECT_EQ(std::memcmp(dest.GetPixels(), src.GetPixels(), src.GetStorageSize()), 0);
  // But the memory should be different
  EXPECT_NE(dest.GetPixels(), src.GetPixels());
}

TEST_F(ImageTest, MoveAssignmentOperator)
{
  const u32 width = 8;
  const u32 height = 6;
  Image src(width, height, ImageFormat::RGBA8);

  // Set a test pattern
  std::memset(src.GetPixels(), 0xCC, src.GetStorageSize());

  // Keep track of the original pointer
  const u8* original_pixels = src.GetPixels();

  // Create a different image first
  Image dest(4, 4, ImageFormat::BGRA8);

  // Assign using move assignment
  dest = std::move(src);

  // Verify properties were moved
  EXPECT_EQ(dest.GetWidth(), width);
  EXPECT_EQ(dest.GetHeight(), height);
  EXPECT_EQ(dest.GetFormat(), ImageFormat::RGBA8);
  EXPECT_EQ(dest.GetPixels(), original_pixels); // Should be the same pointer

  // Source should be invalidated
  EXPECT_FALSE(src.IsValid());
  EXPECT_EQ(src.GetWidth(), 0u);
  EXPECT_EQ(src.GetHeight(), 0u);
  EXPECT_EQ(src.GetFormat(), ImageFormat::None);
  EXPECT_EQ(src.GetPixels(), nullptr);
}

// Test format utility functions
TEST_F(ImageTest, FormatUtilities)
{
  EXPECT_STREQ(Image::GetFormatName(ImageFormat::RGBA8), "RGBA8");
  EXPECT_STREQ(Image::GetFormatName(ImageFormat::BC1), "BC1");

  EXPECT_EQ(Image::GetPixelSize(ImageFormat::RGBA8), 4u);
  EXPECT_EQ(Image::GetPixelSize(ImageFormat::RGB565), 2u);

  EXPECT_FALSE(Image::IsCompressedFormat(ImageFormat::RGBA8));
  EXPECT_TRUE(Image::IsCompressedFormat(ImageFormat::BC1));
}

// Test pixel manipulation
TEST_F(ImageTest, PixelManipulation)
{
  // Check initial pattern
  const u32* first_pixel = reinterpret_cast<const u32*>(m_test_image.GetPixels());
  EXPECT_EQ(*first_pixel, 0xFF000000u); // First pixel should be black with full alpha

  // Test Clear()
  m_test_image.Clear();
  EXPECT_EQ(*first_pixel, 0u);

  // Test SetAllPixelsOpaque()
  std::memset(m_test_image.GetPixels(), 0x0u, m_test_image.GetStorageSize()); // Clear alpha
  m_test_image.SetAllPixelsOpaque();

  // Check all pixels now have alpha set to 0xFF
  for (u32 y = 0; y < m_test_image.GetHeight(); y++)
  {
    for (u32 x = 0; x < m_test_image.GetWidth(); x++)
    {
      const u32* pixel = reinterpret_cast<const u32*>(m_test_image.GetRowPixels(y) + x * sizeof(u32));
      EXPECT_EQ((*pixel & 0xFF000000), 0xFF000000u);
    }
  }
}

// Test resize functionality
TEST_F(ImageTest, Resize)
{
  const u32 new_width = 8;
  const u32 new_height = 10;

  // Test resize without preservation
  m_test_image.Resize(new_width, new_height, false);
  EXPECT_EQ(m_test_image.GetWidth(), new_width);
  EXPECT_EQ(m_test_image.GetHeight(), new_height);

  // Test resize with format change
  m_test_image.Resize(new_width, new_height, ImageFormat::BGRA8, false);
  EXPECT_EQ(m_test_image.GetFormat(), ImageFormat::BGRA8);

  // Fill with a known pattern
  std::memset(m_test_image.GetPixels(), 0xBBu, m_test_image.GetStorageSize());

  // Test resize with preservation
  const u32 final_width = 6;
  const u32 final_height = 7;
  m_test_image.Resize(final_width, final_height, true);

  // First bytes should still be 0xBB
  EXPECT_EQ(m_test_image.GetPixels()[0], 0xBBu);
  EXPECT_EQ(m_test_image.GetWidth(), final_width);
  EXPECT_EQ(m_test_image.GetHeight(), final_height);
}

// Test format conversion - additional formats
TEST_F(ImageTest, RGB565ToRGBA8)
{
  constexpr u32 width = 4;
  constexpr u32 height = 4;
  Image rgb565_image(width, height, ImageFormat::RGB565);

  // Fill with a test pattern - pure red in RGB565 format (0xF800)
  for (u32 y = 0; y < height; y++)
  {
    u16* row = reinterpret_cast<u16*>(rgb565_image.GetRowPixels(y));
    for (u32 x = 0; x < width; x++)
    {
      row[x] = 0xF800; // Red in RGB565
    }
  }

  // Convert to RGBA8
  Error err;
  std::optional<Image> rgba8_image = rgb565_image.ConvertToRGBA8(&err);
  ASSERT_TRUE(rgba8_image.has_value());
  EXPECT_EQ(rgba8_image->GetFormat(), ImageFormat::RGBA8);

  // Check the first pixel - should be red with full alpha
  const u32* first_pixel = reinterpret_cast<const u32*>(rgba8_image->GetPixels());
  // Red component should be close to 0xFF (might be 0xF8 due to precision)
  EXPECT_GE((*first_pixel & 0xFFu), 0xF8u);
  // Green and blue should be 0
  EXPECT_EQ((*first_pixel & 0xFF00u), 0u);
  EXPECT_EQ((*first_pixel & 0xFF0000u), 0u);
  // Alpha should be 0xFF
  EXPECT_EQ((*first_pixel & 0xFF000000u), 0xFF000000u);
}

TEST_F(ImageTest, RGB5A1ToRGBA8)
{
  constexpr u32 width = 4;
  constexpr u32 height = 4;
  Image rgb5a1_image(width, height, ImageFormat::RGB5A1);

  // Fill with a test pattern - green with alpha in RGB5A1 format (0x07C0)
  for (u32 y = 0; y < height; y++)
  {
    u16* row = reinterpret_cast<u16*>(rgb5a1_image.GetRowPixels(y));
    for (u32 x = 0; x < width; x++)
    {
      row[x] = 0x87C0; // Green with alpha bit set
    }
  }

  // Convert to RGBA8
  Error err;
  std::optional<Image> rgba8_image = rgb5a1_image.ConvertToRGBA8(&err);
  ASSERT_TRUE(rgba8_image.has_value());
  EXPECT_EQ(rgba8_image->GetFormat(), ImageFormat::RGBA8);

  // Check the first pixel - should be green with full alpha
  const u32* first_pixel = reinterpret_cast<const u32*>(rgba8_image->GetPixels());
  // Green component should be set, red and blue should be 0
  EXPECT_GE((*first_pixel & 0xFFu), 8u);
  EXPECT_LT((*first_pixel & 0xFFu), 16u);
  EXPECT_GE(((*first_pixel >> 8) & 0xFFu), 0xF0u);
  EXPECT_LT(((*first_pixel >> 8) & 0xFFu), 0xF8u);
  EXPECT_EQ((*first_pixel & 0xFF0000u), 0u);
  // Alpha should be 0xFF since the alpha bit was set
  EXPECT_EQ((*first_pixel & 0xFF000000u), 0xFF000000u);
}

// Test block sizes for compressed formats
TEST_F(ImageTest, BlockSizes)
{
  // Test with 16x16 image - evenly divisible by block size (4)
  const u32 width = 16;
  const u32 height = 16;

  // BC1 format (4x4 blocks, 8 bytes per block)
  Image bc1_image(width, height, ImageFormat::BC1);
  EXPECT_EQ(bc1_image.GetBlocksWide(), width / 4);
  EXPECT_EQ(bc1_image.GetBlocksHigh(), height / 4);
  EXPECT_EQ(bc1_image.GetPitch(), (width / 4) * 8);

  // Test with non-multiple dimensions
  const u32 odd_width = 10;
  const u32 odd_height = 6;

  // BC1 format with non-multiple dimensions
  Image bc1_odd_image(odd_width, odd_height, ImageFormat::BC1);
  // Should round up to multiple of 4
  EXPECT_EQ(bc1_odd_image.GetBlocksWide(), (odd_width + 3) / 4);
  EXPECT_EQ(bc1_odd_image.GetBlocksHigh(), (odd_height + 3) / 4);

  // BC3 format (4x4 blocks, 16 bytes per block)
  Image bc3_image(width, height, ImageFormat::BC3);
  EXPECT_EQ(bc3_image.GetBlocksWide(), width / 4);
  EXPECT_EQ(bc3_image.GetBlocksHigh(), height / 4);
  EXPECT_EQ(bc3_image.GetPitch(), (width / 4) * 16);

  // Storage size test for BC1
  const u32 bc1_storage = bc1_image.GetStorageSize();
  EXPECT_EQ(bc1_storage, (width / 4) * (height / 4) * 8);

  // Storage size test for BC3
  const u32 bc3_storage = bc3_image.GetStorageSize();
  EXPECT_EQ(bc3_storage, (width / 4) * (height / 4) * 16);
}

// Test GetPixelsSpan
TEST_F(ImageTest, PixelSpans)
{
  // Test const span
  std::span<const u8> const_span = m_test_image.GetPixelsSpan();
  EXPECT_EQ(const_span.data(), m_test_image.GetPixels());
  EXPECT_EQ(const_span.size(), m_test_image.GetStorageSize());

  // Test non-const span
  std::span<u8> mutable_span = m_test_image.GetPixelsSpan();
  EXPECT_EQ(mutable_span.data(), m_test_image.GetPixels());
  EXPECT_EQ(mutable_span.size(), m_test_image.GetStorageSize());

  // Modify through the span and verify
  if (!mutable_span.empty())
  {
    mutable_span[0] = 0xAA;
    EXPECT_EQ(m_test_image.GetPixels()[0], 0xAAu);
  }
}

// Test TakePixels
TEST_F(ImageTest, TakePixels)
{
  const u32 width = 8;
  const u32 height = 6;
  Image src(width, height, ImageFormat::RGBA8);

  // Set a test pattern
  std::memset(src.GetPixels(), 0xCC, src.GetStorageSize());

  // Keep track of the original pointer
  const u8* original_pixels = src.GetPixels();

  // Take pixels
  Image::PixelStorage pixels = src.TakePixels();

  // Original image should now be invalid
  EXPECT_FALSE(src.IsValid());
  EXPECT_EQ(src.GetWidth(), 0u);
  EXPECT_EQ(src.GetHeight(), 0u);
  EXPECT_EQ(src.GetFormat(), ImageFormat::None);
  EXPECT_EQ(src.GetPixels(), nullptr);

  // Pixels pointer should be the original pointer
  EXPECT_EQ(pixels.get(), original_pixels);
}

// Test invalid operations
TEST_F(ImageTest, OperationsOnInvalidImage)
{
  Image invalid_image;

  // These operations should safely handle invalid images
  invalid_image.Clear(); // No-op for invalid images
  invalid_image.FlipY(); // No-op for invalid images

  // GetStorageSize should return 0 for invalid image
  EXPECT_EQ(invalid_image.GetStorageSize(), 0u);

  // GetPixels should return nullptr for invalid image
  EXPECT_EQ(invalid_image.GetPixels(), nullptr);

  // Spans should be empty for invalid image
  EXPECT_TRUE(invalid_image.GetPixelsSpan().empty());
}

// Test conversion of different formats to RGBA8
TEST_F(ImageTest, ConvertMultipleFormatsToRGBA8)
{
  const u32 width = 4;
  const u32 height = 4;
  Error err;

  // Test RGBA8 to RGBA8 (should be essentially a copy)
  {
    Image rgba8_image(width, height, ImageFormat::RGBA8);
    std::memset(rgba8_image.GetPixels(), 0xAA, rgba8_image.GetStorageSize());

    std::optional<Image> converted = rgba8_image.ConvertToRGBA8(&err);
    ASSERT_TRUE(converted.has_value());
    EXPECT_EQ(converted->GetFormat(), ImageFormat::RGBA8);
    EXPECT_EQ(std::memcmp(converted->GetPixels(), rgba8_image.GetPixels(), rgba8_image.GetStorageSize()), 0);
  }

  // Test BGRA8 to RGBA8 (color channels should be swapped)
  {
    Image bgra8_image(width, height, ImageFormat::BGRA8);
    // Set to blue in BGRA8 (0xFFRRGGBB = 0xFF0000FF)
    std::fill_n(reinterpret_cast<u32*>(bgra8_image.GetPixels()), width * height, 0xFF0000FF);

    std::optional<Image> converted = bgra8_image.ConvertToRGBA8(&err);
    ASSERT_TRUE(converted.has_value());

    // First pixel should now be red in RGBA8 (0xFFBBGGRR = 0xFFFF0000)
    const u32* first_pixel = reinterpret_cast<const u32*>(converted->GetPixels());
    EXPECT_EQ(*first_pixel, 0xFFFF0000u);
  }

  // Test A1BGR5 to RGBA8
  {
    Image a1bgr5_image(width, height, ImageFormat::A1BGR5);
    // Set to blue with alpha in A1BGR5
    std::fill_n(reinterpret_cast<u16*>(a1bgr5_image.GetPixels()), width * height, static_cast<u16>(0x837b));

    std::optional<Image> converted = a1bgr5_image.ConvertToRGBA8(&err);
    ASSERT_TRUE(converted.has_value());

    // First pixel should now be blue with alpha
    const u32* first_pixel = reinterpret_cast<const u32*>(converted->GetPixels());
    // Red should be high, green/blue should be low
    EXPECT_GE((*first_pixel & 0xFFu), 0x80u);
    EXPECT_GE(((*first_pixel >> 8) & 0xFFu), 0x34u);
    EXPECT_GE(((*first_pixel >> 16) & 0xFFu), 0xE8u);
    // Alpha should be 0xFF
    EXPECT_EQ((*first_pixel & 0xFF000000u), 0xFF000000u);
  }
}

// Test calculation functions
TEST_F(ImageTest, PitchAndStorage)
{
  const u32 width = 16;
  const u32 height = 8;

  // Test uncompressed format
  const u32 rgba_pitch = Image::CalculatePitch(width, height, ImageFormat::RGBA8);
  EXPECT_EQ(rgba_pitch, width * 4); // 4 bytes per pixel

  const u32 rgba_storage = Image::CalculateStorageSize(width, height, ImageFormat::RGBA8);
  EXPECT_EQ(rgba_storage, rgba_pitch * height);

  // Test compressed format (BC1)
  const u32 bc1_pitch = Image::CalculatePitch(width, height, ImageFormat::BC1);
  // BC1 uses 8 bytes per 4x4 block
  EXPECT_EQ(bc1_pitch, (width / 4) * 8);

  const u32 bc1_storage = Image::CalculateStorageSize(width, height, ImageFormat::BC1);
  // Storage should be pitch * number of blocks high
  EXPECT_EQ(bc1_storage, bc1_pitch * (height / 4));
}

// Test flip Y operation
TEST_F(ImageTest, FlipY)
{
  // Create a test image with different colors on top and bottom
  Image test_image(2, 2, ImageFormat::RGBA8);

  // Top row: Red
  u32* top_left = reinterpret_cast<u32*>(test_image.GetRowPixels(0));
  u32* top_right = top_left + 1;
  *top_left = *top_right = 0xFF0000FFu; // Red in RGBA

  // Bottom row: Blue
  u32* bottom_left = reinterpret_cast<u32*>(test_image.GetRowPixels(1));
  u32* bottom_right = bottom_left + 1;
  *bottom_left = *bottom_right = 0xFFFF0000u; // Blue in RGBA

  // Flip the image
  test_image.FlipY();

  // Now the top row should be blue and the bottom row should be red
  top_left = reinterpret_cast<u32*>(test_image.GetRowPixels(0));
  top_right = top_left + 1;
  bottom_left = reinterpret_cast<u32*>(test_image.GetRowPixels(1));
  bottom_right = bottom_left + 1;

  EXPECT_EQ(*top_left, 0xFFFF0000u);     // Blue
  EXPECT_EQ(*top_right, 0xFFFF0000u);    // Blue
  EXPECT_EQ(*bottom_left, 0xFF0000FFu);  // Red
  EXPECT_EQ(*bottom_right, 0xFF0000FFu); // Red
}

// Test edge cases
TEST_F(ImageTest, ZeroDimensions)
{
  // Create image with zero width/height
  Image zero_width(0, 10, ImageFormat::RGBA8);
  EXPECT_FALSE(zero_width.IsValid());

  Image zero_height(10, 0, ImageFormat::RGBA8);
  EXPECT_FALSE(zero_height.IsValid());

  // Resize to zero dimensions
  Image normal(8, 8, ImageFormat::RGBA8);
  normal.Resize(0, 8, false);
  EXPECT_FALSE(normal.IsValid());
}

// Test that Invalidate properly resets all properties
TEST_F(ImageTest, Invalidate)
{
  Image img(16, 16, ImageFormat::RGBA8);
  EXPECT_TRUE(img.IsValid());
  EXPECT_NE(img.GetPixels(), nullptr);

  img.Invalidate();
  EXPECT_FALSE(img.IsValid());
  EXPECT_EQ(img.GetWidth(), 0u);
  EXPECT_EQ(img.GetHeight(), 0u);
  EXPECT_EQ(img.GetFormat(), ImageFormat::None);
  EXPECT_EQ(img.GetPixels(), nullptr);
}
