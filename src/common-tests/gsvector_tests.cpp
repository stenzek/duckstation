// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/gsvector.h"

#include "gtest/gtest.h"

static constexpr s8 s8t(int v)
{
  return static_cast<s8>(static_cast<s32>(v));
}

static constexpr s16 s16t(int v)
{
  return static_cast<s16>(static_cast<s32>(v));
}

// GSVector2i Tests
TEST(GSVector2iTest, Construction)
{
  // Default constructor
  [[maybe_unused]] GSVector2i v1;
  // Values are uninitialized, so we don't test them

  // Single value constructor
  GSVector2i v2(42);
  EXPECT_EQ(v2.x, 42);
  EXPECT_EQ(v2.y, 42);

  // Two value constructor
  GSVector2i v3(10, 20);
  EXPECT_EQ(v3.x, 10);
  EXPECT_EQ(v3.y, 20);

  // 16-bit constructor
  GSVector2i v4(s16(1), s16(2), s16(3), s16(4));
  EXPECT_EQ(v4.S16[0], 1);
  EXPECT_EQ(v4.S16[1], 2);
  EXPECT_EQ(v4.S16[2], 3);
  EXPECT_EQ(v4.S16[3], 4);

  // 8-bit constructor
  GSVector2i v5(s8t(1), s8t(2), s8t(3), s8t(4), s8t(5), s8t(6), s8t(7), s8t(8));
  for (int i = 0; i < 8; i++)
  {
    EXPECT_EQ(v5.S8[i], i + 1);
  }

  // Copy constructor
  GSVector2i v6(v3);
  EXPECT_EQ(v6.x, 10);
  EXPECT_EQ(v6.y, 20);
}

TEST(GSVector2iTest, ConstexprCreation)
{
  constexpr auto v1 = GSVector2i::cxpr(5, 10);
  EXPECT_EQ(v1.x, 5);
  EXPECT_EQ(v1.y, 10);

  constexpr auto v2 = GSVector2i::cxpr(7);
  EXPECT_EQ(v2.x, 7);
  EXPECT_EQ(v2.y, 7);

  constexpr auto v3 = GSVector2i::cxpr16(s16(255));
  for (int i = 0; i < 4; i++)
  {
    EXPECT_EQ(v3.S16[i], 255);
  }
}

TEST(GSVector2iTest, SaturationOperations)
{
  GSVector2i v1(100, 200);
  GSVector2i min_val(-50, -100);
  GSVector2i max_val(150, 250);

  auto sat_result = v1.sat_s32(min_val, max_val);
  EXPECT_EQ(sat_result.x, 100); // Within range
  EXPECT_EQ(sat_result.y, 200); // Within range

  GSVector2i v2(300, -150);
  auto sat_result2 = v2.sat_s32(min_val, max_val);
  EXPECT_EQ(sat_result2.x, 150);  // Clamped to max
  EXPECT_EQ(sat_result2.y, -100); // Clamped to min
}

TEST(GSVector2iTest, MinMaxVertical)
{
  GSVector2i v1(10, 20);

  EXPECT_EQ(v1.minv_s32(), 10);
  EXPECT_EQ(v1.maxv_s32(), 20);
  EXPECT_EQ(v1.addv_s32(), 30);

  GSVector2i v2(s8t(0x50), s8t(0x40), s8t(0x30), s8t(0x20), s8t(0x80), s8t(0x70), s8t(0x60), s8t(0x10)); // 8-bit values
  EXPECT_EQ(v2.minv_u8(), 0x10u);
  EXPECT_EQ(v2.maxv_u8(), 0x80u);

  GSVector2i v3(s16t(0x1000), s16t(0x2000), s16t(0x3000), s16t(0x4000)); // 16-bit values
  EXPECT_EQ(v3.minv_u16(), 0x1000u);
  EXPECT_EQ(v3.maxv_u16(), 0x4000u);
}

TEST(GSVector2iTest, ClampOperations)
{
  // Test clamp8 which does pu16().upl8()
  GSVector2i v1(300, 400, -100, 500); // Values that exceed 8-bit range
  auto clamped = v1.clamp8();
  // This should pack to 16-bit unsigned with saturation, then unpack low 8-bit
  for (int i = 0; i < 8; i++)
  {
    EXPECT_GE(clamped.U8[i], 0);
    EXPECT_LE(clamped.U8[i], 255);
  }
}

TEST(GSVector2iTest, BlendOperations)
{
  GSVector2i v1(s8t(0x11), s8t(0x22), s8t(0x33), s8t(0x44), s8t(0x55), s8t(0x66), s8t(0x77), s8t(0x88));
  GSVector2i v2(s8t(0xAA), s8t(0xBB), s8t(0xCC), s8t(0xDD), s8t(0xEE), s8t(0xFF), s8t(0x00), s8t(0x11));
  GSVector2i mask(s8t(0x80), s8t(0x00), s8t(0x80), s8t(0x00), s8t(0x80), s8t(0x00), s8t(0x80),
                  s8t(0x00)); // Alternate selection

  auto blend_result = v1.blend8(v2, mask);
  EXPECT_EQ(blend_result.U8[0], 0xAAu); // mask bit set, select from v2
  EXPECT_EQ(blend_result.U8[1], 0x22u); // mask bit clear, select from v1
  EXPECT_EQ(blend_result.U8[2], 0xCCu); // mask bit set, select from v2
  EXPECT_EQ(blend_result.U8[3], 0x44u); // mask bit clear, select from v1
}

TEST(GSVector2iTest, BlendTemplated)
{
  GSVector2i v1(s16t(0x1111), s16t(0x2222), s16t(0x3333), s16t(0x4444));
  GSVector2i v2(s16t(0xAAAA), s16t(0xBBBB), s16t(0xCCCC), s16t(0xDDDD));

  // Test blend16 with mask 0x5 (binary 0101) - select elements 0 and 2 from v2
  auto blend16_result = v1.blend16<0x5>(v2);
  EXPECT_EQ(blend16_result.U16[0], 0xAAAAu); // bit 0 set, select from v2
  EXPECT_EQ(blend16_result.U16[1], 0x2222u); // bit 1 clear, select from v1
  EXPECT_EQ(blend16_result.U16[2], 0xCCCCu); // bit 2 set, select from v2
  EXPECT_EQ(blend16_result.U16[3], 0x4444u); // bit 3 clear, select from v1

  // Test blend32 with mask 0x1 - select element 0 from v2
  auto blend32_result = v1.blend32<0x1>(v2);
  EXPECT_EQ(blend32_result.U32[0], 0xBBBBAAAAu); // bit 0 set, select from v2
  EXPECT_EQ(blend32_result.U32[1], 0x44443333u); // bit 1 clear, select from v1
}

TEST(GSVector2iTest, ShuffleOperations)
{
  GSVector2i v1(s8t(0x10), s8t(0x20), s8t(0x30), s8t(0x40), s8t(0x50), s8t(0x60), s8t(0x70), s8t(0x80));
  GSVector2i shuffle_mask(s8t(0x00), s8t(0x02), s8t(0x04), s8t(0x06), s8t(0x80), s8t(0x81), s8t(0x82),
                          s8t(0x83)); // Mix indices and zero

  auto shuffled = v1.shuffle8(shuffle_mask);
  EXPECT_EQ(shuffled.S8[0], s8t(0x10)); // Index 0
  EXPECT_EQ(shuffled.S8[1], s8t(0x30)); // Index 2
  EXPECT_EQ(shuffled.S8[2], s8t(0x50)); // Index 4
  EXPECT_EQ(shuffled.S8[3], s8t(0x70)); // Index 6
  EXPECT_EQ(shuffled.S8[4], s8t(0));    // High bit set, zero
  EXPECT_EQ(shuffled.S8[5], s8t(0));    // High bit set, zero
}

TEST(GSVector2iTest, PackingOperations)
{
  // Test ps16 - pack signed 16-bit to signed 8-bit with saturation
  GSVector2i v1(300, -200, 100, 400); // 16-bit values, some out of 8-bit range
  auto packed_s = v1.ps16();
  EXPECT_EQ(packed_s.S8[0], 127);  // 300 saturated to max s8
  EXPECT_EQ(packed_s.S8[1], -128); // -200 saturated to min s8
  EXPECT_EQ(packed_s.S8[2], 100);  // 100 within range
  EXPECT_EQ(packed_s.S8[3], 127);  // 400 saturated to max s8

  // Test pu16 - pack unsigned 16-bit to unsigned 8-bit with saturation
  GSVector2i v2(100, 300, 50, 400);
  auto packed_u = v2.pu16();
  EXPECT_EQ(packed_u.U8[0], 100); // 100 within range
  EXPECT_EQ(packed_u.U8[1], 255); // 300 saturated to max u8
  EXPECT_EQ(packed_u.U8[2], 50);  // 50 within range
  EXPECT_EQ(packed_u.U8[3], 255); // 400 saturated to max u8
}

TEST(GSVector2iTest, UnpackOperations)
{
  GSVector2i v1(0x12, 0x34, 0x56, 0x78);

  auto upl8_result = v1.upl8();
  EXPECT_EQ(upl8_result.U8[0], 0x12);
  EXPECT_EQ(upl8_result.U8[1], 0);
  EXPECT_EQ(upl8_result.U8[2], 0);
  EXPECT_EQ(upl8_result.U8[3], 0);
  EXPECT_EQ(upl8_result.U8[4], 0x34);
  EXPECT_EQ(upl8_result.U8[5], 0);
  EXPECT_EQ(upl8_result.U8[6], 0);
  EXPECT_EQ(upl8_result.U8[7], 0);

  auto upl16_result = v1.upl16();
  EXPECT_EQ(upl16_result.U16[0], 0x12);
  EXPECT_EQ(upl16_result.U16[1], 0);
  EXPECT_EQ(upl16_result.U16[2], 0x34);
  EXPECT_EQ(upl16_result.U16[3], 0);
}

TEST(GSVector2iTest, TypeConversions)
{
  GSVector2i v1(0x12, 0x34, 0x56, 0x78);

  // Test u8to16
  auto s8to16_result = v1.u8to16();
  EXPECT_EQ(s8to16_result.S16[0], 0x12);
  EXPECT_EQ(s8to16_result.S16[1], 0);
  EXPECT_EQ(s8to16_result.S16[2], 0x34);
  EXPECT_EQ(s8to16_result.S16[3], 0);

  // Test u8to32
  auto u8to32_result = v1.u8to32();
  EXPECT_EQ(u8to32_result.U32[0], 0x12u);
  EXPECT_EQ(u8to32_result.U32[1], 0u);
}

TEST(GSVector2iTest, ByteShifts)
{
  GSVector2i v1(s8t(0x12), s8t(0x34), s8t(0x56), s8t(0x78), s8t(0x9A), s8t(0xBC), s8t(0xDE), s8t(0xF0));

  // Test srl<2> - shift right logical by 2 bytes
  auto srl_result = v1.srl<2>();
  EXPECT_EQ(srl_result.U8[0], 0x56u);
  EXPECT_EQ(srl_result.U8[1], 0x78u);
  EXPECT_EQ(srl_result.U8[2], 0x9Au);
  EXPECT_EQ(srl_result.U8[3], 0xBCu);
  EXPECT_EQ(srl_result.U8[4], 0xDEu);
  EXPECT_EQ(srl_result.U8[5], 0xF0u);
  EXPECT_EQ(srl_result.U8[6], 0u);
  EXPECT_EQ(srl_result.U8[7], 0u);

  // Test sll<3> - shift left logical by 3 bytes
  auto sll_result = v1.sll<3>();
  EXPECT_EQ(sll_result.U8[0], 0u);
  EXPECT_EQ(sll_result.U8[1], 0u);
  EXPECT_EQ(sll_result.U8[2], 0u);
  EXPECT_EQ(sll_result.U8[3], 0x12u);
  EXPECT_EQ(sll_result.U8[4], 0x34u);
  EXPECT_EQ(sll_result.U8[5], 0x56u);
  EXPECT_EQ(sll_result.U8[6], 0x78u);
  EXPECT_EQ(sll_result.U8[7], 0x9Au);
}

TEST(GSVector2iTest, ArithmeticWith16BitElements)
{
  GSVector2i v1(100, 200, 300, 400);
  GSVector2i v2(50, 60, 70, 80);

  auto add16_result = v1.add16(v2);
  EXPECT_EQ(add16_result.S16[0], 150);
  EXPECT_EQ(add16_result.S16[1], 260);
  EXPECT_EQ(add16_result.S16[2], 370);
  EXPECT_EQ(add16_result.S16[3], 480);

  auto sub16_result = v1.sub16(v2);
  EXPECT_EQ(sub16_result.S16[0], 50);
  EXPECT_EQ(sub16_result.S16[1], 140);
  EXPECT_EQ(sub16_result.S16[2], 230);
  EXPECT_EQ(sub16_result.S16[3], 320);

  auto mul16_result = v1.mul16l(v2);
  EXPECT_EQ(mul16_result.S16[0], 5000);
  EXPECT_EQ(mul16_result.S16[1], 12000);
  EXPECT_EQ(mul16_result.S16[2], 21000);
  EXPECT_EQ(mul16_result.S16[3], 32000);
}

TEST(GSVector2iTest, ArithmeticWith8BitElements)
{
  GSVector2i v1(10, 20, 30, 40, 50, 60, 70, 80);
  GSVector2i v2(5, 8, 12, 16, 20, 24, 28, 32);

  auto add8_result = v1.add8(v2);
  for (int i = 0; i < 8; i++)
  {
    EXPECT_EQ(add8_result.S8[i], v1.S8[i] + v2.S8[i]);
  }

  auto sub8_result = v1.sub8(v2);
  for (int i = 0; i < 8; i++)
  {
    EXPECT_EQ(sub8_result.S8[i], v1.S8[i] - v2.S8[i]);
  }
}

TEST(GSVector2iTest, SaturatedArithmetic)
{
  // Test signed saturation
  GSVector2i v1(120, -120, 100, -100, 0, 0, 0, 0);
  GSVector2i v2(50, -50, 60, -60, 0, 0, 0, 0);

  auto adds8_result = v1.adds8(v2);
  EXPECT_EQ(adds8_result.S8[0], 127);  // 120 + 50 = 170, saturated to 127
  EXPECT_EQ(adds8_result.S8[1], -128); // -120 + (-50) = -170, saturated to -128
  EXPECT_EQ(adds8_result.S8[2], 127);  // 100 + 60 = 160, saturated to 127
  EXPECT_EQ(adds8_result.S8[3], -128); // -100 + (-60) = -160, saturated to -128

  auto subs8_result = v1.subs8(v2);
  EXPECT_EQ(subs8_result.S8[0], 70);  // 120 - 50 = 70
  EXPECT_EQ(subs8_result.S8[1], -70); // -120 - (-50) = -70
  EXPECT_EQ(subs8_result.S8[2], 40);  // 100 - 60 = 40
  EXPECT_EQ(subs8_result.S8[3], -40); // -100 - (-60) = -40
}

TEST(GSVector2iTest, UnsignedSaturatedArithmetic)
{
  GSVector2i v1(s8t(200), s8t(100), s8t(150), s8t(50), s8t(0), s8t(0), s8t(0), s8t(0));
  GSVector2i v2(s8t(80), s8t(120), s8t(30), s8t(70), s8t(0), s8t(0), s8t(0), s8t(0));

  auto addus8_result = v1.addus8(v2);
  EXPECT_EQ(addus8_result.U8[0], 255); // 200 + 80 = 280, saturated to 255
  EXPECT_EQ(addus8_result.U8[1], 220); // 100 + 120 = 220
  EXPECT_EQ(addus8_result.U8[2], 180); // 150 + 30 = 180
  EXPECT_EQ(addus8_result.U8[3], 120); // 50 + 70 = 120

  auto subus8_result = v1.subus8(v2);
  EXPECT_EQ(subus8_result.U8[0], 120); // 200 - 80 = 120
  EXPECT_EQ(subus8_result.U8[1], 0);   // 100 - 120 = -20, saturated to 0
  EXPECT_EQ(subus8_result.U8[2], 120); // 150 - 30 = 120
  EXPECT_EQ(subus8_result.U8[3], 0);   // 50 - 70 = -20, saturated to 0
}

TEST(GSVector2iTest, AverageOperations)
{
  GSVector2i v1(s8t(100), s8t(200), s8t(50), s8t(150), s8t(0), s8t(0), s8t(0), s8t(0));
  GSVector2i v2(s8t(80), s8t(180), s8t(70), s8t(130), s8t(0), s8t(0), s8t(0), s8t(0));

  auto avg8_result = v1.avg8(v2);
  EXPECT_EQ(avg8_result.U8[0], 90);  // (100 + 80) / 2 = 90
  EXPECT_EQ(avg8_result.U8[1], 190); // (200 + 180) / 2 = 190
  EXPECT_EQ(avg8_result.U8[2], 60);  // (50 + 70) / 2 = 60
  EXPECT_EQ(avg8_result.U8[3], 140); // (150 + 130) / 2 = 140

  auto avg16_result = v1.avg16(v2);
  EXPECT_EQ(avg16_result.U16[0], (51300 + 46160) / 2); // Average of packed 16-bit values
  EXPECT_EQ(avg16_result.U16[1], (38450 + 33350) / 2);
}

TEST(GSVector2iTest, ComparisonOperations)
{
  GSVector2i v1(10, 20, 30, 40, 50, 60, 70, 80);
  GSVector2i v2(5, 25, 30, 45, 55, 55, 75, 75);

  // Test eq8
  auto eq8_result = v1.eq8(v2);
  EXPECT_EQ(eq8_result.S8[0], 0);  // 10 != 5
  EXPECT_EQ(eq8_result.S8[1], 0);  // 20 != 25
  EXPECT_EQ(eq8_result.S8[2], -1); // 30 == 30
  EXPECT_EQ(eq8_result.S8[3], 0);  // 40 != 45

  // Test neq8
  auto neq8_result = v1.neq8(v2);
  EXPECT_EQ(neq8_result.S8[0], -1); // 10 != 5
  EXPECT_EQ(neq8_result.S8[1], -1); // 20 != 25
  EXPECT_EQ(neq8_result.S8[2], 0);  // 30 == 30
  EXPECT_EQ(neq8_result.S8[3], -1); // 40 != 45

  // Test gt8
  auto gt8_result = v1.gt8(v2);
  EXPECT_EQ(gt8_result.S8[0], -1); // 10 > 5
  EXPECT_EQ(gt8_result.S8[1], 0);  // 20 < 25
  EXPECT_EQ(gt8_result.S8[2], 0);  // 30 == 30
  EXPECT_EQ(gt8_result.S8[3], 0);  // 40 < 45

  // Test ge8
  auto ge8_result = v1.ge8(v2);
  EXPECT_EQ(ge8_result.S8[0], -1); // 10 >= 5
  EXPECT_EQ(ge8_result.S8[1], 0);  // 20 < 25
  EXPECT_EQ(ge8_result.S8[2], -1); // 30 >= 30
  EXPECT_EQ(ge8_result.S8[3], 0);  // 40 < 45

  // Test lt8
  auto lt8_result = v1.lt8(v2);
  EXPECT_EQ(lt8_result.S8[0], 0);  // 10 > 5
  EXPECT_EQ(lt8_result.S8[1], -1); // 20 < 25
  EXPECT_EQ(lt8_result.S8[2], 0);  // 30 == 30
  EXPECT_EQ(lt8_result.S8[3], -1); // 40 < 45

  // Test le8
  auto le8_result = v1.le8(v2);
  EXPECT_EQ(le8_result.S8[0], 0);  // 10 > 5
  EXPECT_EQ(le8_result.S8[1], -1); // 20 <= 25
  EXPECT_EQ(le8_result.S8[2], -1); // 30 <= 30
  EXPECT_EQ(le8_result.S8[3], -1); // 40 <= 45
}

TEST(GSVector2iTest, MaskAndBooleanOperations)
{
  GSVector2i v1(s8t(0x80), s8t(0x40), s8t(0x80), s8t(0x00), s8t(0x80), s8t(0x80), s8t(0x00), s8t(0x80));

  s32 mask_result = v1.mask();
  // Mask should be formed from high bits of each byte
  s32 expected_mask = 0x01 | 0x04 | 0x10 | 0x20 | 0x80; // Bits 0, 2, 4, 5, 7
  EXPECT_EQ(mask_result & 0xB5, expected_mask & 0xB5);  // Check set bits

  // Test alltrue and allfalse
  GSVector2i all_ones;
  all_ones.U64[0] = 0xFFFFFFFFFFFFFFFFULL;
  EXPECT_TRUE(all_ones.alltrue());
  EXPECT_FALSE(all_ones.allfalse());

  GSVector2i all_zeros;
  all_zeros.U64[0] = 0;
  EXPECT_FALSE(all_zeros.alltrue());
  EXPECT_TRUE(all_zeros.allfalse());
}

TEST(GSVector2iTest, InsertExtractOperations)
{
  GSVector2i v1(0x12345678, 0x9ABCDEF0);

  // Test insert/extract 8-bit
  auto v_insert8 = v1.insert8<0>(0x55);
  EXPECT_EQ(v_insert8.extract8<0>(), 0x55);
  EXPECT_EQ(v1.extract8<1>(), s8t(0x56));

  // Test insert/extract 16-bit
  auto v_insert16 = v1.insert16<1>(0x1234);
  EXPECT_EQ(v_insert16.extract16<1>(), 0x1234);
  EXPECT_EQ(v1.extract16<0>(), static_cast<s16>(0x5678));

  // Test insert/extract 32-bit
  auto v_insert32 = v1.insert32<0>(0xAABBCCDD);
  EXPECT_EQ(v_insert32.extract32<0>(), static_cast<s32>(0xAABBCCDD));
  EXPECT_EQ(v1.extract32<1>(), static_cast<s32>(0x9ABCDEF0));
}

TEST(GSVector2iTest, LoadStoreOperations)
{
  // Test load32
  s32 value = 0x12345678;
  auto loaded32 = GSVector2i::load32(&value);
  EXPECT_EQ(loaded32.x, 0x12345678);
  EXPECT_EQ(loaded32.y, 0);

  // Test set32
  auto set32_result = GSVector2i::set32(0xAABBCCDD);
  EXPECT_EQ(set32_result.x, static_cast<s32>(0xAABBCCDD));
  EXPECT_EQ(set32_result.y, 0);

  // Test store32
  s32 output_value;
  GSVector2i::store32(&output_value, loaded32);
  EXPECT_EQ(output_value, 0x12345678);

  // Test full load/store
  s32 data[2] = {0x11111111, 0x22222222};
  auto loaded = GSVector2i::load<true>(data);
  EXPECT_EQ(loaded.S32[0], 0x11111111);
  EXPECT_EQ(loaded.S32[1], 0x22222222);

  s32 output[2];
  GSVector2i::store<true>(output, loaded);
  EXPECT_EQ(output[0], 0x11111111);
  EXPECT_EQ(output[1], 0x22222222);
}

TEST(GSVector2iTest, BitwiseAssignmentOperations)
{
  GSVector2i v1(0xF0F0F0F0, 0x0F0F0F0F);
  GSVector2i v2(0xAAAAAAAA, 0x55555555);

  // Test &=
  GSVector2i v_and = v1;
  v_and &= v2;
  EXPECT_EQ(v_and.U32[0], 0xA0A0A0A0u);
  EXPECT_EQ(v_and.U32[1], 0x05050505u);

  // Test |=
  GSVector2i v_or = v1;
  v_or |= v2;
  EXPECT_EQ(v_or.U32[0], 0xFAFAFAFAu);
  EXPECT_EQ(v_or.U32[1], 0x5F5F5F5Fu);

  // Test ^=
  GSVector2i v_xor = v1;
  v_xor ^= v2;
  EXPECT_EQ(v_xor.U32[0], 0x5A5A5A5Au);
  EXPECT_EQ(v_xor.U32[1], 0x5A5A5A5Au);
}

TEST(GSVector2iTest, BitwiseScalarOperations)
{
  GSVector2i v1(0xF0F0F0F0, 0x0F0F0F0F);
  s32 scalar = 0xAAAAAAAA;

  auto and_result = v1 & scalar;
  EXPECT_EQ(and_result.U32[0], 0xA0A0A0A0u);
  EXPECT_EQ(and_result.U32[1], 0x0A0A0A0Au);

  auto or_result = v1 | scalar;
  EXPECT_EQ(or_result.U32[0], 0xFAFAFAFAu);
  EXPECT_EQ(or_result.U32[1], 0xAFAFAFAFu);

  auto xor_result = v1 ^ scalar;
  EXPECT_EQ(xor_result.U32[0], 0x5A5A5A5Au);
  EXPECT_EQ(xor_result.U32[1], 0xA5A5A5A5u);
}

TEST(GSVector2iTest, NotOperation)
{
  GSVector2i v1(0xF0F0F0F0, 0x0F0F0F0F);
  auto not_result = ~v1;
  EXPECT_EQ(not_result.U32[0], 0x0F0F0F0Fu);
  EXPECT_EQ(not_result.U32[1], 0xF0F0F0F0u);
}

// GSVector2 Tests
TEST(GSVector2Test, Construction)
{
  // Single value constructor
  GSVector2 v1(3.14f);
  EXPECT_FLOAT_EQ(v1.x, 3.14f);
  EXPECT_FLOAT_EQ(v1.y, 3.14f);

  // Two value constructor (float)
  GSVector2 v2(1.5f, 2.5f);
  EXPECT_FLOAT_EQ(v2.x, 1.5f);
  EXPECT_FLOAT_EQ(v2.y, 2.5f);

  // Two value constructor (int)
  GSVector2 v3(10, 20);
  EXPECT_FLOAT_EQ(v3.x, 10.0f);
  EXPECT_FLOAT_EQ(v3.y, 20.0f);

  // Single int constructor
  GSVector2 v4(42);
  EXPECT_FLOAT_EQ(v4.x, 42.0f);
  EXPECT_FLOAT_EQ(v4.y, 42.0f);
}

TEST(GSVector2Test, BlendOperations)
{
  GSVector2 v1(1.0f, 2.0f);
  GSVector2 v2(3.0f, 4.0f);

  // Test templated blend32
  auto blend_result = v1.blend32<1>(v2); // mask = 1, select x from v1, y from v2
  EXPECT_FLOAT_EQ(blend_result.x, 3.0f); // From v2
  EXPECT_FLOAT_EQ(blend_result.y, 2.0f); // From v1

  // Test mask-based blend32
  GSVector2 mask;
  mask.U32[0] = 0x80000000u; // High bit set
  mask.U32[1] = 0x00000000u; // High bit clear
  auto mask_blend_result = v1.blend32(v2, mask);
  EXPECT_FLOAT_EQ(mask_blend_result.x, 3.0f); // From v2 (mask high bit set)
  EXPECT_FLOAT_EQ(mask_blend_result.y, 2.0f); // From v1 (mask high bit clear)
}

TEST(GSVector2Test, MaskOperations)
{
  GSVector2 v1;
  v1.U32[0] = 0x80000000u; // High bit set
  v1.U32[1] = 0x40000000u; // Second bit set

  int mask_result = v1.mask();
  EXPECT_EQ(mask_result, 0x1); // One bit should be set in result
}

TEST(GSVector2Test, ReplaceNaN)
{
  GSVector2 v1(1.0f, std::numeric_limits<float>::quiet_NaN());
  GSVector2 replacement(99.0f, 88.0f);

  auto result = v1.replace_nan(replacement);
  EXPECT_FLOAT_EQ(result.x, 1.0f);  // Not NaN, keep original
  EXPECT_FLOAT_EQ(result.y, 88.0f); // Was NaN, use replacement
}

TEST(GSVector2Test, InsertExtract)
{
  GSVector2 v1(1.0f, 2.0f);
  GSVector2 v2(3.0f, 4.0f);

  // Test insert32
  auto insert_result = v1.insert32<1, 0>(v2); // Insert v2[1] into v1[0]
  EXPECT_FLOAT_EQ(insert_result.x, 4.0f);     // v2[1]
  EXPECT_FLOAT_EQ(insert_result.y, 2.0f);     // Original v1[1]

  // Test extract32
  GSVector2 v3;
  v3.I32[0] = 0x12345678;
  v3.I32[1] = 0x9ABCDEF0;
  EXPECT_EQ(v3.extract32<0>(), 0x12345678);
  EXPECT_EQ(v3.extract32<1>(), static_cast<s32>(0x9ABCDEF0));
}

TEST(GSVector2Test, ComparisonOperators)
{
  GSVector2 v1(1.0f, 2.0f);
  GSVector2 v2(1.0f, 3.0f);

  auto eq_result = v1 == v2;
  EXPECT_EQ(eq_result.I32[0], -1); // 1.0 == 1.0
  EXPECT_EQ(eq_result.I32[1], 0);  // 2.0 != 3.0

  auto neq_result = v1 != v2;
  EXPECT_EQ(neq_result.I32[0], 0);  // 1.0 == 1.0
  EXPECT_EQ(neq_result.I32[1], -1); // 2.0 != 3.0

  auto gt_result = v1 > v2;
  EXPECT_EQ(gt_result.I32[0], 0); // 1.0 == 1.0
  EXPECT_EQ(gt_result.I32[1], 0); // 2.0 < 3.0

  auto lt_result = v1 < v2;
  EXPECT_EQ(lt_result.I32[0], 0);  // 1.0 == 1.0
  EXPECT_EQ(lt_result.I32[1], -1); // 2.0 < 3.0

  auto ge_result = v1 >= v2;
  EXPECT_EQ(ge_result.I32[0], -1); // 1.0 >= 1.0
  EXPECT_EQ(ge_result.I32[1], 0);  // 2.0 < 3.0

  auto le_result = v1 <= v2;
  EXPECT_EQ(le_result.I32[0], -1); // 1.0 <= 1.0
  EXPECT_EQ(le_result.I32[1], -1); // 2.0 <= 3.0
}

TEST(GSVector2Test, XfffffffffConstant)
{
  const auto all_ones = GSVector2::xffffffff();
  EXPECT_EQ(all_ones.U64[0], 0xFFFFFFFFFFFFFFFFULL);
}

// GSVector4i Tests
TEST(GSVector4iTest, ConstexprCreation8Bit)
{
  constexpr auto v1 = GSVector4i::cxpr8(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
  for (int i = 0; i < 16; i++)
  {
    EXPECT_EQ(v1.S8[i], i + 1);
  }
}

TEST(GSVector4iTest, MaddOperations)
{
  GSVector4i v1(1, 2, 3, 4, 5, 6, 7, 8); // 16-bit values
  GSVector4i v2(2, 3, 4, 5, 6, 7, 8, 9);

  auto madd_result = v1.madd_s16(v2);
  EXPECT_EQ(madd_result.S32[0], 1 * 2 + 2 * 3); // 2 + 6 = 8
  EXPECT_EQ(madd_result.S32[1], 3 * 4 + 4 * 5); // 12 + 20 = 32
  EXPECT_EQ(madd_result.S32[2], 5 * 6 + 6 * 7); // 30 + 42 = 72
  EXPECT_EQ(madd_result.S32[3], 7 * 8 + 8 * 9); // 56 + 72 = 128
}

TEST(GSVector4iTest, HorizontalAdd)
{
  GSVector4i v1(10, 20, 30, 40);

  auto addp_result = v1.addp_s32();
  EXPECT_EQ(addp_result.x, 30); // 10 + 20
  EXPECT_EQ(addp_result.y, 70); // 30 + 40
  EXPECT_EQ(addp_result.z, 30);
  EXPECT_EQ(addp_result.w, 70);
}

TEST(GSVector4iTest, VerticalMinMaxU8)
{
  GSVector4i v1(s8t(10), s8t(50), s8t(30), s8t(200), s8t(15), s8t(100), s8t(5), s8t(250), s8t(80), s8t(20), s8t(60),
                s8t(40), s8t(90), s8t(70), s8t(180), s8t(1));

  EXPECT_EQ(v1.minv_u8(), 1u);
  EXPECT_EQ(v1.maxv_u8(), 250u);
}

TEST(GSVector4iTest, VerticalMinMaxU16)
{
  GSVector4i v1(1000, 2000, 500, 3000, 1500, 800, 2500, 100);

  EXPECT_EQ(v1.minv_u16(), 100u);
  EXPECT_EQ(v1.maxv_u16(), 3000u);
}

TEST(GSVector4iTest, HaddS16)
{
  GSVector4i v1(10, 20, 30, 40, 50, 60, 70, 80);
  GSVector4i v2(1, 2, 3, 4, 5, 6, 7, 8);

  auto hadds_result = v1.hadds16(v2);
  // First vector: pairs (10,20), (30,40), (50,60), (70,80)
  // Second vector: pairs (1,2), (3,4), (5,6), (7,8)
  EXPECT_EQ(hadds_result.S16[0], 30);  // 10 + 20
  EXPECT_EQ(hadds_result.S16[1], 70);  // 30 + 40
  EXPECT_EQ(hadds_result.S16[2], 110); // 50 + 60
  EXPECT_EQ(hadds_result.S16[3], 150); // 70 + 80
  EXPECT_EQ(hadds_result.S16[4], 3);   // 1 + 2
  EXPECT_EQ(hadds_result.S16[5], 7);   // 3 + 4
  EXPECT_EQ(hadds_result.S16[6], 11);  // 5 + 6
  EXPECT_EQ(hadds_result.S16[7], 15);  // 7 + 8
}

TEST(GSVector4iTest, BlendOperationsAdvanced)
{
  GSVector4i v1(s8t(0x11), s8t(0x22), s8t(0x33), s8t(0x44), s8t(0x55), s8t(0x66), s8t(0x77), s8t(0x88), s8t(0x99),
                s8t(0xAA), s8t(0xBB), s8t(0xCC), s8t(0xDD), s8t(0xEE), s8t(0xFF), s8t(0x00));
  GSVector4i v2(s8t(0xA1), s8t(0xB2), s8t(0xC3), s8t(0xD4), s8t(0xE5), s8t(0xF6), s8t(0x07), s8t(0x18), s8t(0x29),
                s8t(0x3A), s8t(0x4B), s8t(0x5C), s8t(0x6D), s8t(0x7E), s8t(0x8F), s8t(0x90));
  GSVector4i mask_blend(s8t(0xFF), s8t(0x00), s8t(0xFF), s8t(0x00), s8t(0xFF), s8t(0x00), s8t(0xFF), s8t(0x00),
                        s8t(0xFF), s8t(0x00), s8t(0xFF), s8t(0x00), s8t(0xFF), s8t(0x00), s8t(0xFF), s8t(0x00));

  auto blend_result = v1.blend(v2, mask_blend);
  // The blend operation should mix bits based on the mask
  // Where mask bit is 1, select from v2; where 0, select from v1
  for (int i = 0; i < 2; i++)
  {
    u64 expected = (v2.U64[i] & mask_blend.U64[i]) | (v1.U64[i] & ~mask_blend.U64[i]);
    EXPECT_EQ(blend_result.U64[i], expected);
  }
}

TEST(GSVector4iTest, AdvancedPackingWithTwoVectors)
{
  GSVector4i v1(100, 200, 300, 400); // 32-bit signed values
  GSVector4i v2(500, 600, 700, 800);

  auto ps32_result = v1.ps32(v2);
  // Should pack both vectors' 32-bit values to 16-bit with saturation
  EXPECT_EQ(ps32_result.S16[0], 100);
  EXPECT_EQ(ps32_result.S16[1], 200);
  EXPECT_EQ(ps32_result.S16[2], 300);
  EXPECT_EQ(ps32_result.S16[3], 400);
  EXPECT_EQ(ps32_result.S16[4], 500);
  EXPECT_EQ(ps32_result.S16[5], 600);
  EXPECT_EQ(ps32_result.S16[6], 700);
  EXPECT_EQ(ps32_result.S16[7], 800);
}

TEST(GSVector4iTest, AdvancedUnpackingWithTwoVectors)
{
  GSVector4i v1(s8t(0x11), s8t(0x22), s8t(0x33), s8t(0x44), s8t(0x55), s8t(0x66), s8t(0x77), s8t(0x88), s8t(0x99),
                s8t(0xAA), s8t(0xBB), s8t(0xCC), s8t(0xDD), s8t(0xEE), s8t(0xFF), s8t(0x00));
  GSVector4i v2(s8t(0xA1), s8t(0xB2), s8t(0xC3), s8t(0xD4), s8t(0xE5), s8t(0xF6), s8t(0x07), s8t(0x18), s8t(0x29),
                s8t(0x3A), s8t(0x4B), s8t(0x5C), s8t(0x6D), s8t(0x7E), s8t(0x8F), s8t(0x90));

  auto upl8_result = v1.upl8(v2);
  // Should interleave low 8 bytes from both vectors
  EXPECT_EQ(upl8_result.S8[0], s8t(0x11)); // v1[0]
  EXPECT_EQ(upl8_result.S8[1], s8t(0xA1)); // v2[0]
  EXPECT_EQ(upl8_result.S8[2], s8t(0x22)); // v1[1]
  EXPECT_EQ(upl8_result.S8[3], s8t(0xB2)); // v2[1]

  auto uph8_result = v1.uph8(v2);
  // Should interleave high 8 bytes from both vectors
  EXPECT_EQ(uph8_result.S8[0], s8t(0x99)); // v1[8]
  EXPECT_EQ(uph8_result.S8[1], s8t(0x29)); // v2[8]
  EXPECT_EQ(uph8_result.S8[2], s8t(0xAA)); // v1[9]
  EXPECT_EQ(uph8_result.S8[3], s8t(0x3A)); // v2[9]
}

TEST(GSVector4iTest, Type64BitConversions)
{
  GSVector4i v1(s8t(0x12), s8t(0x34), s8t(0x56), s8t(0x78), s8t(0x9A), s8t(0xBC), s8t(0xDE), s8t(0xF0), s8t(0x11),
                s8t(0x22), s8t(0x33), s8t(0x44), s8t(0x55), s8t(0x66), s8t(0x77), s8t(0x88));

  // Test s8to64
  auto s8to64_result = v1.s8to64();
  EXPECT_EQ(s8to64_result.S64[0], 0x12);
  EXPECT_EQ(s8to64_result.S64[1], 0x34);

  // Test u16to64
  auto u16to64_result = v1.u16to64();
  EXPECT_EQ(u16to64_result.U64[0], 0x3412u); // Little endian 16-bit
  EXPECT_EQ(u16to64_result.U64[1], 0x7856u);

  // Test s32to64
  auto s32to64_result = v1.s32to64();
  EXPECT_EQ(s32to64_result.S64[0], static_cast<s64>(0x0000000078563412LL));
  EXPECT_EQ(s32to64_result.S64[1], static_cast<s64>(0xFFFFFFFFF0DEBC9ALL));
}

TEST(GSVector4iTest, Shift64BitOperations)
{
  GSVector4i v1;
  v1.U64[0] = 0x123456789ABCDEF0ULL;
  v1.U64[1] = 0xFEDCBA0987654321ULL;

  // Test sll64
  auto sll64_result = v1.sll64<4>();
  EXPECT_EQ(sll64_result.U64[0], 0x23456789ABCDEF00ULL);
  EXPECT_EQ(sll64_result.U64[1], 0xEDCBA09876543210ULL);

  // Test srl64
  auto srl64_result = v1.srl64<4>();
  EXPECT_EQ(srl64_result.U64[0], 0x0123456789ABCDEFULL);
  EXPECT_EQ(srl64_result.U64[1], 0x0FEDCBA098765432ULL);
}

#ifdef GSVECTOR_HAS_SRLV
TEST(GSVector4iTest, VariableShifts)
{
  GSVector4i v1(0x1000, 0x2000, 0x4000, 0x8000, 0x1000, 0x2000, 0x4000, 0x8000);
  GSVector4i shift_amounts(1, 2, 3, 4, 1, 2, 3, 4);

  auto sllv16_result = v1.sllv16(shift_amounts);
  EXPECT_EQ(sllv16_result.U16[0], 0x2000); // 0x1000 << 1
  EXPECT_EQ(sllv16_result.U16[1], 0x8000); // 0x2000 << 2
  EXPECT_EQ(sllv16_result.U16[2], 0x0000); // 0x4000 << 3 (overflow)
  EXPECT_EQ(sllv16_result.U16[3], 0x0000); // 0x8000 << 4 (overflow)
  EXPECT_EQ(sllv16_result.U16[4], 0x2000); // 0x1000 << 1
  EXPECT_EQ(sllv16_result.U16[5], 0x8000); // 0x2000 << 2
  EXPECT_EQ(sllv16_result.U16[6], 0x0000); // 0x4000 << 3 (overflow)
  EXPECT_EQ(sllv16_result.U16[7], 0x0000); // 0x8000 << 4 (overflow)

  auto srlv16_result = v1.srlv16(shift_amounts);
  EXPECT_EQ(srlv16_result.U16[0], 0x0800); // 0x1000 >> 1
  EXPECT_EQ(srlv16_result.U16[1], 0x0800); // 0x2000 >> 2
  EXPECT_EQ(srlv16_result.U16[2], 0x0800); // 0x4000 >> 3
  EXPECT_EQ(srlv16_result.U16[3], 0x0800); // 0x8000 >> 4
  EXPECT_EQ(srlv16_result.U16[4], 0x0800); // 0x1000 >> 1
  EXPECT_EQ(srlv16_result.U16[5], 0x0800); // 0x2000 >> 2
  EXPECT_EQ(srlv16_result.U16[6], 0x0800); // 0x4000 >> 3
  EXPECT_EQ(srlv16_result.U16[7], 0x0800); // 0x8000 >> 4
}
#endif

TEST(GSVector4iTest, MultiplicationOperations)
{
  GSVector4i v1(10, 20, 30, 40, 50, 60, 70, 80);
  GSVector4i v2(2, 3, 4, 5, 6, 7, 8, 9);

  // Test mul16hs - high 16 bits of 16-bit multiplication
  auto mul16hs_result = v1.mul16hs(v2);
  // For 16-bit values, this should mostly be 0 unless we have large values
  for (int i = 0; i < 8; i++)
  {
    s32 expected = (v1.S16[i] * v2.S16[i]) >> 16;
    EXPECT_EQ(mul16hs_result.S16[i], expected);
  }

  // Test mul16hrs - rounded high 16 bits
  auto mul16hrs_result = v1.mul16hrs(v2);
  for (int i = 0; i < 8; i++)
  {
    const s16 expected = static_cast<s16>((((v1.S16[i] * v2.S16[i]) >> 14) + 1) >> 1);
    EXPECT_EQ(mul16hrs_result.S16[i], expected);
  }
}

TEST(GSVector4iTest, Eq64Operations)
{
  GSVector4i v1;
  GSVector4i v2;
  v1.S64[0] = 0x123456789ABCDEF0LL;
  v1.S64[1] = 0xFEDCBA0987654321LL;
  v2.S64[0] = 0x123456789ABCDEF0LL; // Same as v1[0]
  v2.S64[1] = 0x1111111111111111LL; // Different from v1[1]

  auto eq64_result = v1.eq64(v2);
  EXPECT_EQ(eq64_result.S64[0], -1); // Equal
  EXPECT_EQ(eq64_result.S64[1], 0);  // Not equal
}

TEST(GSVector4iTest, InsertExtract64Bit)
{
  GSVector4i v1(0x12345678, 0x9ABCDEF0, 0x11111111, 0x22222222);

  // Test insert64
  auto v_insert64 = v1.insert64<0>(static_cast<s64>(0x9999888877776666ULL));
  EXPECT_EQ(v_insert64.extract64<0>(), static_cast<s64>(0x9999888877776666ULL));
  EXPECT_EQ(v_insert64.extract64<1>(), v1.extract64<1>());

  // Test extract64
  EXPECT_EQ(v1.extract64<0>(), static_cast<s64>(0x9ABCDEF012345678ULL)); // Little endian combination
  EXPECT_EQ(v1.extract64<1>(), static_cast<s64>(0x2222222211111111ULL));
}

TEST(GSVector4iTest, LoadStoreSpecialOperations)
{
  // Test loadnt (non-temporal load)
  alignas(VECTOR_ALIGNMENT) static constexpr const s32 data[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
  auto loaded_nt = GSVector4i::loadnt(data);
  for (int i = 0; i < 4; i++)
  {
    EXPECT_EQ(loaded_nt.S32[i], data[i]);
  }

  // Test storent (non-temporal store)
  alignas(VECTOR_ALIGNMENT) s32 output_nt[4];
  GSVector4i::storent(output_nt, loaded_nt);
  for (int i = 0; i < 4; i++)
  {
    EXPECT_EQ(output_nt[i], data[i]);
  }

  // Test zext32
  auto zext_result = GSVector4i::zext32(0x12345678);
  EXPECT_EQ(zext_result.x, 0x12345678);
  EXPECT_EQ(zext_result.y, 0);
  EXPECT_EQ(zext_result.z, 0);
  EXPECT_EQ(zext_result.w, 0);
}

TEST(GSVector4iTest, LoadStoreHalfOperations)
{
  // Test loadl and loadh
  u64 data[2] = {0x123456789ABCDEF0ULL, 0xFEDCBA0987654321ULL};

  auto loaded_low = GSVector4i::loadl<true>(data);
  EXPECT_EQ(loaded_low.U64[0], data[0]);
  EXPECT_EQ(loaded_low.U64[1], 0u);

  auto loaded_high = GSVector4i::loadh<true>(data);
  EXPECT_EQ(loaded_high.U64[0], 0u);
  EXPECT_EQ(loaded_high.U64[1], data[0]);

  // Test storel and storeh
  GSVector4i test_vec;
  test_vec.U64[0] = 0xAAAABBBBCCCCDDDDULL;
  test_vec.U64[1] = 0xEEEEFFFF00001111ULL;

  s32 output_low[2];
  GSVector4i::storel<true>(output_low, test_vec);
  EXPECT_EQ(reinterpret_cast<u64*>(output_low)[0], test_vec.U64[0]);

  s32 output_high[2];
  GSVector4i::storeh<true>(output_high, test_vec);
  EXPECT_EQ(reinterpret_cast<u64*>(output_high)[0], test_vec.U64[1]);
}

TEST(GSVector4iTest, BroadcastOperations)
{
  GSVector4i v1(10, 20, 30, 40);

  auto broadcast_result = GSVector4i::broadcast128(v1);
  // In no-SIMD implementation, this just returns the same vector
  EXPECT_EQ(broadcast_result.x, v1.x);
  EXPECT_EQ(broadcast_result.y, v1.y);
  EXPECT_EQ(broadcast_result.z, v1.z);
  EXPECT_EQ(broadcast_result.w, v1.w);
}

TEST(GSVector4iTest, StaticHelperFunctions)
{
  GSVector2i xy(10, 20);
  GSVector2i zw(30, 40);

  auto xyxy_result1 = GSVector4i::xyxy(xy, zw);
  EXPECT_EQ(xyxy_result1.x, 10);
  EXPECT_EQ(xyxy_result1.y, 20);
  EXPECT_EQ(xyxy_result1.z, 30);
  EXPECT_EQ(xyxy_result1.w, 40);

  auto xyxy_result2 = GSVector4i::xyxy(xy);
  EXPECT_EQ(xyxy_result2.x, 10);
  EXPECT_EQ(xyxy_result2.y, 20);
  EXPECT_EQ(xyxy_result2.z, 10);
  EXPECT_EQ(xyxy_result2.w, 20);
}

// GSVector4 Tests
TEST(GSVector4Test, DoubleOperations)
{
  // Test all 64-bit double operations
  GSVector4 v1 = GSVector4::f64(3.14159, 2.71828);
  GSVector4 v2 = GSVector4::f64(1.41421, 1.73205);

  auto add64_result = v1.add64(v2);
  EXPECT_DOUBLE_EQ(add64_result.F64[0], 3.14159 + 1.41421);
  EXPECT_DOUBLE_EQ(add64_result.F64[1], 2.71828 + 1.73205);

  auto sub64_result = v1.sub64(v2);
  EXPECT_DOUBLE_EQ(sub64_result.F64[0], 3.14159 - 1.41421);
  EXPECT_DOUBLE_EQ(sub64_result.F64[1], 2.71828 - 1.73205);

  auto mul64_result = v1.mul64(v2);
  EXPECT_DOUBLE_EQ(mul64_result.F64[0], 3.14159 * 1.41421);
  EXPECT_DOUBLE_EQ(mul64_result.F64[1], 2.71828 * 1.73205);

  auto div64_result = v1.div64(v2);
  EXPECT_DOUBLE_EQ(div64_result.F64[0], 3.14159 / 1.41421);
  EXPECT_DOUBLE_EQ(div64_result.F64[1], 2.71828 / 1.73205);
}

TEST(GSVector4Test, BasicOps)
{
  GSVector4 v(1.0f, -2.0f, 3.5f, -4.5f);

  EXPECT_FLOAT_EQ(v.addv(), (1.0f - 2.0f + 3.5f - 4.5f));
  EXPECT_FLOAT_EQ(v.minv(), -4.5f);
  EXPECT_FLOAT_EQ(v.maxv(), 3.5f);

  auto av = v.abs();
  EXPECT_FLOAT_EQ(av.x, 1.0f);
  EXPECT_FLOAT_EQ(av.y, 2.0f);
  EXPECT_FLOAT_EQ(av.z, 3.5f);
  EXPECT_FLOAT_EQ(av.w, 4.5f);

  auto nv = v.neg();
  EXPECT_FLOAT_EQ(nv.x, -1.0f);
  EXPECT_FLOAT_EQ(nv.y, 2.0f);
  EXPECT_FLOAT_EQ(nv.z, -3.5f);
  EXPECT_FLOAT_EQ(nv.w, 4.5f);

  auto fl = GSVector4(1.9f, -1.2f, 3.01f, -3.99f).floor();
  EXPECT_FLOAT_EQ(fl.x, 1.0f);
  EXPECT_FLOAT_EQ(fl.y, -2.0f);
  EXPECT_FLOAT_EQ(fl.z, 3.0f);
  EXPECT_FLOAT_EQ(fl.w, -4.0f);

  auto cl = GSVector4(1.1f, -1.2f, 3.01f, -3.99f).ceil();
  EXPECT_FLOAT_EQ(cl.x, 2.0f);
  EXPECT_FLOAT_EQ(cl.y, -1.0f);
  EXPECT_FLOAT_EQ(cl.z, 4.0f);
  EXPECT_FLOAT_EQ(cl.w, -3.0f);

  // sat(scale)
  auto sat_scaled = GSVector4(-5.0f, 10.0f, 500.0f, 260.0f).sat(255.0f);
  EXPECT_FLOAT_EQ(sat_scaled.x, 0.0f);
  EXPECT_FLOAT_EQ(sat_scaled.y, 10.0f);
  EXPECT_FLOAT_EQ(sat_scaled.z, 255.0f);
  EXPECT_FLOAT_EQ(sat_scaled.w, 255.0f);

  // sat(minmax vector) : x/z clamped to [min.x, min.z], y/w to [min.y, min.w]
  GSVector4 range(0.0f, -1.0f, 2.0f, 1.0f);
  auto sat_pair = v.sat(range);
  EXPECT_FLOAT_EQ(sat_pair.x, 1.0f);  // within [0,2]
  EXPECT_FLOAT_EQ(sat_pair.y, -1.0f); // clamped to -1
  EXPECT_FLOAT_EQ(sat_pair.z, 2.0f);  // clamped to 2
  EXPECT_FLOAT_EQ(sat_pair.w, -1.0f); // clamped to -1
}

TEST(GSVector4Test, BlendAndMask)
{
  GSVector4 a(1, 2, 3, 4);
  GSVector4 b(5, 6, 7, 8);

  // Template blend32<mask> (selects only lanes 0/1 from the 'v' argument per bit)
  auto tb = a.blend32<0b1010>(b);
  EXPECT_FLOAT_EQ(tb.x, 1.0f); // bit0 = 0 -> v[0]
  EXPECT_FLOAT_EQ(tb.y, 6.0f); // bit1 = 1 -> v[1]
  EXPECT_FLOAT_EQ(tb.z, 3.0f); // bit2 = 0 -> v[0]
  EXPECT_FLOAT_EQ(tb.w, 8.0f); // bit3 = 1 -> v[1]

  // Masked blend: high bit set -> take from second vector argument (b); else from 'a'
  GSVector4 mask;
  mask.U32[0] = 0x00000000u;
  mask.U32[1] = 0x80000000u;
  mask.U32[2] = 0x00000000u;
  mask.U32[3] = 0x80000000u;
  auto mb = a.blend32(b, mask);
  EXPECT_FLOAT_EQ(mb.x, a.x);
  EXPECT_FLOAT_EQ(mb.y, b.y);
  EXPECT_FLOAT_EQ(mb.z, a.z);
  EXPECT_FLOAT_EQ(mb.w, b.w);

  // mask() bit packing (bits 31,23,15,7)
  GSVector4 m;
  m.U32[0] = 0x80000000u; // sets bit 0
  m.U32[1] = 0x40000000u; // sets bit 1
  m.U32[2] = 0x20000000u; // sets bit 2
  m.U32[3] = 0x10000000u; // sets bit 3
  EXPECT_EQ(m.mask(), 0x1);
}

TEST(GSVector4Test, HorizontalAndInterleave)
{
  GSVector4 v(1, 2, 10, 20);
  auto hadd0 = v.hadd();
  EXPECT_FLOAT_EQ(hadd0.x, 3);
  EXPECT_FLOAT_EQ(hadd0.y, 30);
  EXPECT_FLOAT_EQ(hadd0.z, 3);
  EXPECT_FLOAT_EQ(hadd0.w, 30);

  auto hsub0 = v.hsub();
  EXPECT_FLOAT_EQ(hsub0.x, -1);
  EXPECT_FLOAT_EQ(hsub0.y, -10);
  EXPECT_FLOAT_EQ(hsub0.z, -1);
  EXPECT_FLOAT_EQ(hsub0.w, -10);

  GSVector4 v2(3, 4, 5, 6);
  auto hadd1 = v.hadd(v2);
  EXPECT_FLOAT_EQ(hadd1.x, 3);
  EXPECT_FLOAT_EQ(hadd1.y, 30);
  EXPECT_FLOAT_EQ(hadd1.z, 7);
  EXPECT_FLOAT_EQ(hadd1.w, 11);

  auto hsub1 = v.hsub(v2);
  EXPECT_FLOAT_EQ(hsub1.x, -1);
  EXPECT_FLOAT_EQ(hsub1.y, -10);
  EXPECT_FLOAT_EQ(hsub1.z, -1);
  EXPECT_FLOAT_EQ(hsub1.w, -1);

  // Interleave / low-high helpers
  GSVector4 a(1, 2, 3, 4);
  GSVector4 b(5, 6, 7, 8);
  auto upl = a.upl(b);
  EXPECT_FLOAT_EQ(upl.x, 1);
  EXPECT_FLOAT_EQ(upl.y, 5);
  EXPECT_FLOAT_EQ(upl.z, 2);
  EXPECT_FLOAT_EQ(upl.w, 6);
  auto uph = a.uph(b);
  EXPECT_FLOAT_EQ(uph.x, 3);
  EXPECT_FLOAT_EQ(uph.y, 7);
  EXPECT_FLOAT_EQ(uph.z, 4);
  EXPECT_FLOAT_EQ(uph.w, 8);
  auto l2h = a.l2h(b);
  EXPECT_FLOAT_EQ(l2h.x, 1);
  EXPECT_FLOAT_EQ(l2h.y, 2);
  EXPECT_FLOAT_EQ(l2h.z, 5);
  EXPECT_FLOAT_EQ(l2h.w, 6);
  auto h2l = a.h2l(b);
  EXPECT_FLOAT_EQ(h2l.x, 7);
  EXPECT_FLOAT_EQ(h2l.y, 8);
  EXPECT_FLOAT_EQ(h2l.z, 3);
  EXPECT_FLOAT_EQ(h2l.w, 4);
}

TEST(GSVector4Test, BroadcastAndInsertExtract)
{
  GSVector4 v(9, 2, 3, 4);
  auto bc_self = v.broadcast32();
  EXPECT_FLOAT_EQ(bc_self.x, 9);
  EXPECT_FLOAT_EQ(bc_self.y, 9);
  EXPECT_FLOAT_EQ(bc_self.z, 9);
  EXPECT_FLOAT_EQ(bc_self.w, 9);

  auto bc_static = GSVector4::broadcast32(v);
  EXPECT_FLOAT_EQ(bc_static.z, 9);

  GSVector4 a(1, 2, 3, 4);
  GSVector4 b(5, 6, 7, 8);
  auto ins_from_other = a.insert32<2, 0>(b); // copy b.z into a.x
  EXPECT_FLOAT_EQ(ins_from_other.x, 7);
  EXPECT_FLOAT_EQ(ins_from_other.y, 2);
  EXPECT_FLOAT_EQ(ins_from_other.z, 3);
  EXPECT_FLOAT_EQ(ins_from_other.w, 4);

  auto ins_scalar = a.insert32<1>(42.0f);
  EXPECT_FLOAT_EQ(ins_scalar.x, 1);
  EXPECT_FLOAT_EQ(ins_scalar.y, 42.0f);
  EXPECT_FLOAT_EQ(ins_scalar.z, 3);
  EXPECT_FLOAT_EQ(ins_scalar.w, 4);

  EXPECT_FLOAT_EQ(a.extract32<0>(), 1.0f);
  EXPECT_FLOAT_EQ(a.extract32<3>(), 4.0f);
}

TEST(GSVector4Test, BitwiseAndAndNot)
{
  GSVector4 a;
  a.U32[0] = 0xFFFFFFFFu;
  a.U32[1] = 0x00FF00FFu;
  a.U32[2] = 0x12345678u;
  a.U32[3] = 0xAAAAAAAAu;

  GSVector4 b;
  b.U32[0] = 0x0F0F0F0Fu;
  b.U32[1] = 0xFF00FF00u;
  b.U32[2] = 0xFFFFFFFFu;
  b.U32[3] = 0x55555555u;

  auto vand = a & b;
  EXPECT_EQ(vand.U32[0], 0x0F0F0F0Fu);
  EXPECT_EQ(vand.U32[1], 0x00000000u);
  EXPECT_EQ(vand.U32[2], 0x12345678u);
  EXPECT_EQ(vand.U32[3], 0x00000000u);

  auto vor = a | b;
  EXPECT_EQ(vor.U32[0], 0xFFFFFFFFu);
  EXPECT_EQ(vor.U32[1], 0xFFFFFFFFu);
  EXPECT_EQ(vor.U32[2], 0xFFFFFFFFu);
  EXPECT_EQ(vor.U32[3], 0xFFFFFFFFu);

  auto vxor = a ^ b;
  EXPECT_EQ(vxor.U32[0], 0xF0F0F0F0u);
  EXPECT_EQ(vxor.U32[1], 0xFFFFFFFFu);
  EXPECT_EQ(vxor.U32[2], 0xEDCBA987u);
  EXPECT_EQ(vxor.U32[3], 0xFFFFFFFFu);

  auto an = a.andnot(b); // (~b) & a
  EXPECT_EQ(an.U32[0], (~b.U32[0]) & a.U32[0]);
  EXPECT_EQ(an.U32[1], (~b.U32[1]) & a.U32[1]);
  EXPECT_EQ(an.U32[2], (~b.U32[2]) & a.U32[2]);
  EXPECT_EQ(an.U32[3], (~b.U32[3]) & a.U32[3]);
}

TEST(GSVector4Test, ReplaceNaN)
{
  GSVector4 v(1.0f, std::numeric_limits<float>::quiet_NaN(), -5.0f, std::numeric_limits<float>::quiet_NaN());
  GSVector4 repl(10.0f, 20.0f, 30.0f, 40.0f);
  auto r = v.replace_nan(repl);
  EXPECT_FLOAT_EQ(r.x, 1.0f);  // kept
  EXPECT_FLOAT_EQ(r.y, 20.0f); // replaced
  EXPECT_FLOAT_EQ(r.z, -5.0f); // kept
  EXPECT_FLOAT_EQ(r.w, 40.0f); // replaced
}

TEST(GSVector4Test, DoubleExtendedOps)
{
  GSVector4 d = GSVector4::f64(-4.0, 9.0);
  auto sq = d.sqr64();
  EXPECT_DOUBLE_EQ(sq.F64[0], 16.0);
  EXPECT_DOUBLE_EQ(sq.F64[1], 81.0);

  auto rt = GSVector4::f64(4.0, 9.0).sqrt64();
  EXPECT_DOUBLE_EQ(rt.F64[0], 2.0);
  EXPECT_DOUBLE_EQ(rt.F64[1], 3.0);

  auto ab = d.abs64();
  EXPECT_DOUBLE_EQ(ab.F64[0], 4.0);
  EXPECT_DOUBLE_EQ(ab.F64[1], 9.0);

  auto ng = d.neg64();
  EXPECT_DOUBLE_EQ(ng.F64[0], 4.0);
  EXPECT_DOUBLE_EQ(ng.F64[1], -9.0);

  GSVector4 d2 = GSVector4::f64(-2.0, 10.0);
  EXPECT_DOUBLE_EQ(d.min64(d2).F64[0], -4.0);
  EXPECT_DOUBLE_EQ(d.min64(d2).F64[1], 9.0);
  EXPECT_DOUBLE_EQ(d.max64(d2).F64[0], -2.0);
  EXPECT_DOUBLE_EQ(d.max64(d2).F64[1], 10.0);

  auto gt = d.gt64(d2);
  EXPECT_EQ(gt.U64[0], 0ULL); // -4 > -2 ? no
  EXPECT_EQ(gt.U64[1], 0ULL); // 9 > 10 ? no
  auto lt = d.lt64(d2);
  EXPECT_NE(lt.U64[0], 0ULL); // -4 < -2
  EXPECT_NE(lt.U64[1], 0ULL); // 9 < 10
  auto ge = d.ge64(d2);
  EXPECT_EQ(ge.U64[0], 0ULL); // -4 >= -2 ? no
  EXPECT_EQ(ge.U64[1], 0ULL); // 9 >= 10 ? no
  auto le = d.le64(d2);
  EXPECT_NE(le.U64[0], 0ULL);
  EXPECT_NE(le.U64[1], 0ULL);
  auto eq = d.eq64(d);
  EXPECT_EQ(eq.U64[0], 0xFFFFFFFFFFFFFFFFULL);
  EXPECT_EQ(eq.U64[1], 0xFFFFFFFFFFFFFFFFULL);
}

TEST(GSVector4Test, FloatToDoubleConversions)
{
  GSVector4 vf(1.25f, 2.75f, 3.0f, 4.0f);
  auto fd = GSVector4::f32to64(vf);
  EXPECT_DOUBLE_EQ(fd.F64[0], 1.25);
  EXPECT_DOUBLE_EQ(fd.F64[1], 2.75);

  GSVector4 vd = GSVector4::f64(5.9, -2.1);
  auto i32 = vd.f64toi32();
  EXPECT_EQ(i32.S32[0], 5);
  EXPECT_EQ(i32.S32[1], -2);
}

// Cross-class conversion tests
TEST(GSVectorTest, ConversionsGSVector2iGSVector2)
{
  GSVector2i vi(42, 84);
  GSVector2 vf(vi);
  EXPECT_FLOAT_EQ(vf.x, 42.0f);
  EXPECT_FLOAT_EQ(vf.y, 84.0f);

  GSVector2 vf2(3.14f, 2.71f);
  GSVector2i vi2(vf2);
  EXPECT_EQ(vi2.x, 3);
  EXPECT_EQ(vi2.y, 2);

  // Test cast operations
  auto cast_result = GSVector2::cast(vi);
  // Cast preserves bit pattern, so we can't directly compare float values
  EXPECT_EQ(cast_result.I32[0], 42);
  EXPECT_EQ(cast_result.I32[1], 84);

  auto cast_result2 = GSVector2i::cast(vf2);
  // Cast preserves bit pattern
  EXPECT_EQ(cast_result2.U32[0], vf2.U32[0]);
  EXPECT_EQ(cast_result2.U32[1], vf2.U32[1]);
}

// width() tests
TEST(GSVectorTest, Width_ReturnsCorrectValue)
{
  const GSVector4 rect1 = GSVector4(10.0f, 20.0f, 50.0f, 80.0f); // left=10, top=20, right=50, bottom=80
  const GSVector4 rect2 = GSVector4(30.0f, 40.0f, 100.0f, 120.0f);

  EXPECT_FLOAT_EQ(rect1.width(), 40.0f); // 50 - 10 = 40
  EXPECT_FLOAT_EQ(rect2.width(), 70.0f); // 100 - 30 = 70
}

TEST(GSVectorTest, Width_EmptyRect_ReturnsZero)
{
  const GSVector4 emptyRect = GSVector4(0.0f, 0.0f, 0.0f, 0.0f);
  const GSVector4 zeroSizeRect = GSVector4(25.0f, 35.0f, 25.0f, 35.0f);

  EXPECT_FLOAT_EQ(emptyRect.width(), 0.0f);
  EXPECT_FLOAT_EQ(zeroSizeRect.width(), 0.0f);
}

TEST(GSVectorTest, Width_InvalidRect_ReturnsNegative)
{
  const GSVector4 invalidRect = GSVector4(50.0f, 80.0f, 10.0f, 20.0f); // right < left, bottom < top

  EXPECT_FLOAT_EQ(invalidRect.width(), -40.0f); // 10 - 50 = -40
}

// height() tests
TEST(GSVectorTest, Height_ReturnsCorrectValue)
{
  const GSVector4 rect1 = GSVector4(10.0f, 20.0f, 50.0f, 80.0f); // left=10, top=20, right=50, bottom=80
  const GSVector4 rect2 = GSVector4(30.0f, 40.0f, 100.0f, 120.0f);

  EXPECT_FLOAT_EQ(rect1.height(), 60.0f); // 80 - 20 = 60
  EXPECT_FLOAT_EQ(rect2.height(), 80.0f); // 120 - 40 = 80
}

TEST(GSVectorTest, Height_EmptyRect_ReturnsZero)
{
  const GSVector4 emptyRect = GSVector4(0.0f, 0.0f, 0.0f, 0.0f);
  const GSVector4 zeroSizeRect = GSVector4(25.0f, 35.0f, 25.0f, 35.0f);

  EXPECT_FLOAT_EQ(emptyRect.height(), 0.0f);
  EXPECT_FLOAT_EQ(zeroSizeRect.height(), 0.0f);
}

TEST(GSVectorTest, Height_InvalidRect_ReturnsNegative)
{
  const GSVector4 invalidRect = GSVector4(50.0f, 80.0f, 10.0f, 20.0f); // right < left, bottom < top

  EXPECT_FLOAT_EQ(invalidRect.height(), -60.0f); // 20 - 80 = -60
}

// rsize() tests
TEST(GSVectorTest, Rsize_ReturnsCorrectSize)
{
  const GSVector4 rect1 = GSVector4(10.0f, 20.0f, 50.0f, 80.0f); // left=10, top=20, right=50, bottom=80
  const GSVector4 rect2 = GSVector4(30.0f, 40.0f, 100.0f, 120.0f);

  GSVector2 size1 = rect1.rsize();
  EXPECT_FLOAT_EQ(size1.x, 40.0f);
  EXPECT_FLOAT_EQ(size1.y, 60.0f);

  GSVector2 size2 = rect2.rsize();
  EXPECT_FLOAT_EQ(size2.x, 70.0f);
  EXPECT_FLOAT_EQ(size2.y, 80.0f);
}

TEST(GSVectorTest, Rsize_EmptyRect_ReturnsZeroSize)
{
  const GSVector4 emptyRect = GSVector4(0.0f, 0.0f, 0.0f, 0.0f);

  GSVector2 size = emptyRect.rsize();
  EXPECT_FLOAT_EQ(size.x, 0.0f);
  EXPECT_FLOAT_EQ(size.y, 0.0f);
}

// rvalid() tests
TEST(GSVectorTest, Rvalid_ValidRect_ReturnsTrue)
{
  const GSVector4 rect1 = GSVector4(10.0f, 20.0f, 50.0f, 80.0f); // left=10, top=20, right=50, bottom=80
  const GSVector4 rect2 = GSVector4(30.0f, 40.0f, 100.0f, 120.0f);
  EXPECT_TRUE(rect1.rvalid());
  EXPECT_TRUE(rect2.rvalid());
}

TEST(GSVectorTest, Rvalid_EmptyRect_ReturnsFalse)
{
  const GSVector4 emptyRect = GSVector4(0.0f, 0.0f, 0.0f, 0.0f);
  const GSVector4 zeroSizeRect = GSVector4(25.0f, 35.0f, 25.0f, 35.0f);

  // Empty rect where left==right and top==bottom is considered invalid
  EXPECT_FALSE(emptyRect.rvalid());
  EXPECT_FALSE(zeroSizeRect.rvalid());
}

TEST(GSVectorTest, Rvalid_InvalidRect_ReturnsFalse)
{
  const GSVector4 invalidRect = GSVector4(50.0f, 80.0f, 10.0f, 20.0f); // right < left, bottom < top
  EXPECT_FALSE(invalidRect.rvalid());
}

TEST(GSVectorTest, Rvalid_PartiallyInvalid_ReturnsFalse)
{
  const GSVector4 invalidWidth(50.0f, 20.0f, 10.0f, 80.0f);  // right < left
  const GSVector4 invalidHeight(10.0f, 80.0f, 50.0f, 20.0f); // bottom < top

  EXPECT_FALSE(invalidWidth.rvalid());
  EXPECT_FALSE(invalidHeight.rvalid());
}

// rempty() tests
TEST(GSVectorTest, Rempty_EmptyRect_ReturnsTrue)
{
  const GSVector4 emptyRect = GSVector4(0.0f, 0.0f, 0.0f, 0.0f);
  const GSVector4 zeroSizeRect = GSVector4(25.0f, 35.0f, 25.0f, 35.0f);
  EXPECT_TRUE(emptyRect.rempty());
  EXPECT_TRUE(zeroSizeRect.rempty());
}

TEST(GSVectorTest, Rempty_NonEmptyRect_ReturnsFalse)
{
  const GSVector4 rect1 = GSVector4(10.0f, 20.0f, 50.0f, 80.0f); // left=10, top=20, right=50, bottom=80
  const GSVector4 rect2 = GSVector4(30.0f, 40.0f, 100.0f, 120.0f);
  EXPECT_FALSE(rect1.rempty());
  EXPECT_FALSE(rect2.rempty());
}

TEST(GSVectorTest, Rempty_ZeroWidthOnly_ReturnsTrue)
{
  GSVector4 zeroWidth(25.0f, 20.0f, 25.0f, 80.0f); // width = 0, height > 0
  EXPECT_TRUE(zeroWidth.rempty());
}

TEST(GSVectorTest, Rempty_ZeroHeightOnly_ReturnsTrue)
{
  GSVector4 zeroHeight(10.0f, 50.0f, 40.0f, 50.0f); // width > 0, height = 0
  EXPECT_TRUE(zeroHeight.rempty());
}

TEST(GSVectorTest, Rempty_InvalidRect_ReturnsTrue)
{
  const GSVector4 invalidRect = GSVector4(50.0f, 80.0f, 10.0f, 20.0f); // right < left, bottom < top

  // Invalid rects are considered empty
  EXPECT_TRUE(invalidRect.rempty());
}

// runion() tests
TEST(GSVectorTest, Runion_OverlappingRects_ReturnsCorrectUnion)
{
  const GSVector4 rect1 = GSVector4(10.0f, 20.0f, 50.0f, 80.0f); // left=10, top=20, right=50, bottom=80
  const GSVector4 rect2 = GSVector4(30.0f, 40.0f, 100.0f, 120.0f);

  GSVector4 result = rect1.runion(rect2);

  // Union should be min of lefts/tops and max of rights/bottoms
  EXPECT_FLOAT_EQ(result.left, 10.0f);    // min(10, 30)
  EXPECT_FLOAT_EQ(result.top, 20.0f);     // min(20, 40)
  EXPECT_FLOAT_EQ(result.right, 100.0f);  // max(50, 100)
  EXPECT_FLOAT_EQ(result.bottom, 120.0f); // max(80, 120)
}

TEST(GSVectorTest, Runion_NonOverlappingRects_ReturnsEnclosingRect)
{
  GSVector4 rectA(0.0f, 0.0f, 10.0f, 10.0f);
  GSVector4 rectB(50.0f, 50.0f, 60.0f, 60.0f);

  GSVector4 result = rectA.runion(rectB);

  EXPECT_FLOAT_EQ(result.left, 0.0f);
  EXPECT_FLOAT_EQ(result.top, 0.0f);
  EXPECT_FLOAT_EQ(result.right, 60.0f);
  EXPECT_FLOAT_EQ(result.bottom, 60.0f);
}

TEST(GSVectorTest, Runion_ContainedRect_ReturnsOuterRect)
{
  GSVector4 outer(0.0f, 0.0f, 100.0f, 100.0f);
  GSVector4 inner(25.0f, 25.0f, 75.0f, 75.0f);

  GSVector4 result = outer.runion(inner);

  EXPECT_FLOAT_EQ(result.left, 0.0f);
  EXPECT_FLOAT_EQ(result.top, 0.0f);
  EXPECT_FLOAT_EQ(result.right, 100.0f);
  EXPECT_FLOAT_EQ(result.bottom, 100.0f);
}

TEST(GSVectorTest, Runion_WithEmptyRect_ReturnsOtherRect)
{
  const GSVector4 rect1 = GSVector4(10.0f, 20.0f, 50.0f, 80.0f); // left=10, top=20, right=50, bottom=80
  const GSVector4 rect2 = GSVector4(30.0f, 40.0f, 100.0f, 120.0f);
  const GSVector4 emptyRect = GSVector4(0.0f, 0.0f, 0.0f, 0.0f);

  GSVector4 result = rect1.runion(emptyRect);

  // Union with empty rect at origin
  EXPECT_FLOAT_EQ(result.left, 0.0f);    // min(10, 0)
  EXPECT_FLOAT_EQ(result.top, 0.0f);     // min(20, 0)
  EXPECT_FLOAT_EQ(result.right, 50.0f);  // max(50, 0)
  EXPECT_FLOAT_EQ(result.bottom, 80.0f); // max(80, 0)
}

TEST(GSVectorTest, Runion_IdenticalRects_ReturnsSameRect)
{
  const GSVector4 rect1 = GSVector4(10.0f, 20.0f, 50.0f, 80.0f); // left=10, top=20, right=50, bottom=80

  GSVector4 result = rect1.runion(rect1);

  EXPECT_FLOAT_EQ(result.left, rect1.left);
  EXPECT_FLOAT_EQ(result.top, rect1.top);
  EXPECT_FLOAT_EQ(result.right, rect1.right);
  EXPECT_FLOAT_EQ(result.bottom, rect1.bottom);
}

TEST(GSVectorTest, Runion_NegativeCoordinates_ReturnsCorrectUnion)
{
  GSVector4 rectA(-50.0f, -40.0f, -10.0f, -5.0f);
  GSVector4 rectB(-30.0f, -20.0f, 10.0f, 15.0f);

  GSVector4 result = rectA.runion(rectB);

  EXPECT_FLOAT_EQ(result.left, -50.0f);  // min(-50, -30)
  EXPECT_FLOAT_EQ(result.top, -40.0f);   // min(-40, -20)
  EXPECT_FLOAT_EQ(result.right, 10.0f);  // max(-10, 10)
  EXPECT_FLOAT_EQ(result.bottom, 15.0f); // max(-5, 15)
}

TEST(GSVectorTest, Runion_IsCommutative)
{
  const GSVector4 rect1 = GSVector4(10.0f, 20.0f, 50.0f, 80.0f); // left=10, top=20, right=50, bottom=80
  const GSVector4 rect2 = GSVector4(30.0f, 40.0f, 100.0f, 120.0f);

  GSVector4 result1 = rect1.runion(rect2);
  GSVector4 result2 = rect2.runion(rect1);

  EXPECT_TRUE(result1.eq(result2));
}
