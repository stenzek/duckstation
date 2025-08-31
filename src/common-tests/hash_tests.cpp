// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/md5_digest.h"
#include "common/sha1_digest.h"
#include "common/sha256_digest.h"

#include <gtest/gtest.h>

TEST(SHA256Digest, Simple)
{
  // https://github.com/B-Con/crypto-algorithms/blob/master/sha256_test.c

  static constexpr const char text1[] = "abc";
  static constexpr const char text2[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  static constexpr const char text3[] = "aaaaaaaaaa";

  static constexpr SHA256Digest::Digest hash1 = {{0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                                  0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                                  0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad}};
  static constexpr SHA256Digest::Digest hash2 = {{0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26,
                                                  0x93, 0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff,
                                                  0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1}};
  static constexpr SHA256Digest::Digest hash3 = {{0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92, 0x81, 0xa1, 0xc7,
                                                  0xe2, 0x84, 0xd7, 0x3e, 0x67, 0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97,
                                                  0x20, 0x0e, 0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0}};

  ASSERT_EQ(SHA256Digest::GetDigest(text1, std::size(text1) - 1), hash1);
  ASSERT_EQ(SHA256Digest::GetDigest(text2, std::size(text2) - 1), hash2);

  SHA256Digest ldigest;
  for (u32 i = 0; i < 100000; i++)
    ldigest.Update(text3, std::size(text3) - 1);

  ASSERT_EQ(ldigest.Final(), hash3);
}

// MD5 Digest Tests
TEST(MD5Digest, EmptyString)
{
  // MD5 hash of empty string: d41d8cd98f00b204e9800998ecf8427e
  static constexpr std::array<u8, MD5Digest::DIGEST_SIZE> expected = {
    {0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04, 0xe9, 0x80, 0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e}};

  const std::string empty_string = "";
  auto result = MD5Digest::HashData(std::span<const u8>(reinterpret_cast<const u8*>(empty_string.data()), 0));
  EXPECT_EQ(result, expected);
}

TEST(MD5Digest, SingleCharacter)
{
  // MD5 hash of "a": 0cc175b9c0f1b6a831c399e269772661
  static constexpr std::array<u8, MD5Digest::DIGEST_SIZE> expected = {
    {0x0c, 0xc1, 0x75, 0xb9, 0xc0, 0xf1, 0xb6, 0xa8, 0x31, 0xc3, 0x99, 0xe2, 0x69, 0x77, 0x26, 0x61}};

  const std::string test_string = "a";
  auto result =
    MD5Digest::HashData(std::span<const u8>(reinterpret_cast<const u8*>(test_string.data()), test_string.size()));
  EXPECT_EQ(result, expected);
}

TEST(MD5Digest, ABC)
{
  // MD5 hash of "abc": 900150983cd24fb0d6963f7d28e17f72
  static constexpr std::array<u8, MD5Digest::DIGEST_SIZE> expected = {
    {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72}};

  const std::string test_string = "abc";
  auto result =
    MD5Digest::HashData(std::span<const u8>(reinterpret_cast<const u8*>(test_string.data()), test_string.size()));
  EXPECT_EQ(result, expected);
}

TEST(MD5Digest, MessageDigest)
{
  // MD5 hash of "message digest": f96b697d7cb7938d525a2f31aaf161d0
  static constexpr std::array<u8, MD5Digest::DIGEST_SIZE> expected = {
    {0xf9, 0x6b, 0x69, 0x7d, 0x7c, 0xb7, 0x93, 0x8d, 0x52, 0x5a, 0x2f, 0x31, 0xaa, 0xf1, 0x61, 0xd0}};

  const std::string test_string = "message digest";
  auto result =
    MD5Digest::HashData(std::span<const u8>(reinterpret_cast<const u8*>(test_string.data()), test_string.size()));
  EXPECT_EQ(result, expected);
}

TEST(MD5Digest, Alphabet)
{
  // MD5 hash of "abcdefghijklmnopqrstuvwxyz": c3fcd3d76192e4007dfb496cca67e13b
  static constexpr std::array<u8, MD5Digest::DIGEST_SIZE> expected = {
    {0xc3, 0xfc, 0xd3, 0xd7, 0x61, 0x92, 0xe4, 0x00, 0x7d, 0xfb, 0x49, 0x6c, 0xca, 0x67, 0xe1, 0x3b}};

  const std::string test_string = "abcdefghijklmnopqrstuvwxyz";
  auto result =
    MD5Digest::HashData(std::span<const u8>(reinterpret_cast<const u8*>(test_string.data()), test_string.size()));
  EXPECT_EQ(result, expected);
}

TEST(MD5Digest, AlphaNumeric)
{
  // MD5 hash of "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789": d174ab98d277d9f5a5611c2c9f419d9f
  static constexpr std::array<u8, MD5Digest::DIGEST_SIZE> expected = {
    {0xd1, 0x74, 0xab, 0x98, 0xd2, 0x77, 0xd9, 0xf5, 0xa5, 0x61, 0x1c, 0x2c, 0x9f, 0x41, 0x9d, 0x9f}};

  const std::string test_string = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  auto result =
    MD5Digest::HashData(std::span<const u8>(reinterpret_cast<const u8*>(test_string.data()), test_string.size()));
  EXPECT_EQ(result, expected);
}

TEST(MD5Digest, LongString)
{
  // MD5 hash of 1000000 'a' characters: 7707d6ae4e027c70eea2a935c2296f21
  static constexpr std::array<u8, MD5Digest::DIGEST_SIZE> expected = {
    {0x77, 0x07, 0xd6, 0xae, 0x4e, 0x02, 0x7c, 0x70, 0xee, 0xa2, 0xa9, 0x35, 0xc2, 0x29, 0x6f, 0x21}};

  MD5Digest digest;
  const char single_char = 'a';

  for (int i = 0; i < 1000000; i++)
  {
    digest.Update(&single_char, 1);
  }

  std::array<u8, MD5Digest::DIGEST_SIZE> result;
  digest.Final(result);
  EXPECT_EQ(result, expected);
}

TEST(MD5Digest, IncrementalUpdate)
{
  // Test that incremental updates produce the same result as a single update
  const std::string test_string = "The quick brown fox jumps over the lazy dog";

  // Single update
  auto result1 =
    MD5Digest::HashData(std::span<const u8>(reinterpret_cast<const u8*>(test_string.data()), test_string.size()));

  // Incremental updates
  MD5Digest digest;
  digest.Update("The quick ", 10);
  digest.Update("brown fox ", 10);
  digest.Update("jumps over ", 11);
  digest.Update("the lazy dog", 12);

  std::array<u8, MD5Digest::DIGEST_SIZE> result2;
  digest.Final(result2);

  EXPECT_EQ(result1, result2);
}

TEST(MD5Digest, Reset)
{
  MD5Digest digest;
  const std::string test_string = "test data";

  // First computation
  digest.Update(test_string.data(), static_cast<u32>(test_string.size()));
  std::array<u8, MD5Digest::DIGEST_SIZE> result1;
  digest.Final(result1);

  // Reset and compute again
  digest.Reset();
  digest.Update(test_string.data(), static_cast<u32>(test_string.size()));
  std::array<u8, MD5Digest::DIGEST_SIZE> result2;
  digest.Final(result2);

  EXPECT_EQ(result1, result2);
}

TEST(MD5Digest, SpanInterface)
{
  const std::string test_string = "test with span interface";
  std::span<const u8> test_span(reinterpret_cast<const u8*>(test_string.data()), test_string.size());

  // Test Update with span
  MD5Digest digest;
  digest.Update(test_span);
  std::array<u8, MD5Digest::DIGEST_SIZE> result1;
  digest.Final(result1);

  // Test HashData with span
  auto result2 = MD5Digest::HashData(test_span);

  EXPECT_EQ(result1, result2);
}

// SHA1 Digest Tests
TEST(SHA1Digest, EmptyString)
{
  // SHA1 hash of empty string: da39a3ee5e6b4b0d3255bfef95601890afd80709
  static constexpr std::array<u8, SHA1Digest::DIGEST_SIZE> expected = {{0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b,
                                                                        0x0d, 0x32, 0x55, 0xbf, 0xef, 0x95, 0x60,
                                                                        0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09}};

  auto result = SHA1Digest::GetDigest("", 0);
  EXPECT_EQ(result, expected);
}

TEST(SHA1Digest, ABC)
{
  // SHA1 hash of "abc": a9993e364706816aba3e25717850c26c9cd0d89d
  static constexpr std::array<u8, SHA1Digest::DIGEST_SIZE> expected = {{0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81,
                                                                        0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50,
                                                                        0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d}};

  const std::string test_string = "abc";
  auto result = SHA1Digest::GetDigest(test_string.data(), test_string.size());
  EXPECT_EQ(result, expected);
}

TEST(SHA1Digest, ABCExtended)
{
  // SHA1 hash of "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq": 84983e441c3bd26ebaae4aa1f95129e5e54670f1
  static constexpr std::array<u8, SHA1Digest::DIGEST_SIZE> expected = {{0x84, 0x98, 0x3e, 0x44, 0x1c, 0x3b, 0xd2,
                                                                        0x6e, 0xba, 0xae, 0x4a, 0xa1, 0xf9, 0x51,
                                                                        0x29, 0xe5, 0xe5, 0x46, 0x70, 0xf1}};

  const std::string test_string = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  auto result = SHA1Digest::GetDigest(test_string.data(), test_string.size());
  EXPECT_EQ(result, expected);
}

TEST(SHA1Digest, QuickBrownFox)
{
  // SHA1 hash of "The quick brown fox jumps over the lazy dog": 2fd4e1c67a2d28fced849ee1bb76e7391b93eb12
  static constexpr std::array<u8, SHA1Digest::DIGEST_SIZE> expected = {{0x2f, 0xd4, 0xe1, 0xc6, 0x7a, 0x2d, 0x28,
                                                                        0xfc, 0xed, 0x84, 0x9e, 0xe1, 0xbb, 0x76,
                                                                        0xe7, 0x39, 0x1b, 0x93, 0xeb, 0x12}};

  const std::string test_string = "The quick brown fox jumps over the lazy dog";
  auto result = SHA1Digest::GetDigest(test_string.data(), test_string.size());
  EXPECT_EQ(result, expected);
}

TEST(SHA1Digest, LongString)
{
  // SHA1 hash of 1000000 'a' characters: 34aa973cd4c4daa4f61eeb2bdbad27316534016f
  static constexpr std::array<u8, SHA1Digest::DIGEST_SIZE> expected = {{0x34, 0xaa, 0x97, 0x3c, 0xd4, 0xc4, 0xda,
                                                                        0xa4, 0xf6, 0x1e, 0xeb, 0x2b, 0xdb, 0xad,
                                                                        0x27, 0x31, 0x65, 0x34, 0x01, 0x6f}};

  SHA1Digest digest;
  const char single_char = 'a';

  for (int i = 0; i < 1000000; i++)
  {
    digest.Update(&single_char, 1);
  }

  u8 result[SHA1Digest::DIGEST_SIZE];
  digest.Final(result);

  std::array<u8, SHA1Digest::DIGEST_SIZE> result_array;
  std::copy(std::begin(result), std::end(result), result_array.begin());

  EXPECT_EQ(result_array, expected);
}

TEST(SHA1Digest, IncrementalUpdate)
{
  // Test that incremental updates produce the same result as a single update
  const std::string test_string = "The quick brown fox jumps over the lazy dog";

  // Single update
  auto result1 = SHA1Digest::GetDigest(test_string.data(), test_string.size());

  // Incremental updates
  SHA1Digest digest;
  digest.Update("The quick ", 10);
  digest.Update("brown fox ", 10);
  digest.Update("jumps over ", 11);
  digest.Update("the lazy dog", 12);

  u8 result_raw[SHA1Digest::DIGEST_SIZE];
  digest.Final(result_raw);

  std::array<u8, SHA1Digest::DIGEST_SIZE> result2;
  std::copy(std::begin(result_raw), std::end(result_raw), result2.begin());

  EXPECT_EQ(result1, result2);
}

TEST(SHA1Digest, Reset)
{
  SHA1Digest digest;
  const std::string test_string = "test data for reset";

  // First computation
  digest.Update(test_string.data(), test_string.size());
  u8 result1_raw[SHA1Digest::DIGEST_SIZE];
  digest.Final(result1_raw);

  std::array<u8, SHA1Digest::DIGEST_SIZE> result1;
  std::copy(std::begin(result1_raw), std::end(result1_raw), result1.begin());

  // Reset and compute again
  digest.Reset();
  digest.Update(test_string.data(), test_string.size());
  u8 result2_raw[SHA1Digest::DIGEST_SIZE];
  digest.Final(result2_raw);

  std::array<u8, SHA1Digest::DIGEST_SIZE> result2;
  std::copy(std::begin(result2_raw), std::end(result2_raw), result2.begin());

  EXPECT_EQ(result1, result2);
}

TEST(SHA1Digest, SpanInterface)
{
  const std::string test_string = "test with span interface";
  std::span<const u8> test_span(reinterpret_cast<const u8*>(test_string.data()), test_string.size());

  // Test Update with span
  SHA1Digest digest;
  digest.Update(test_span);
  u8 result1_raw[SHA1Digest::DIGEST_SIZE];
  digest.Final(result1_raw);

  std::array<u8, SHA1Digest::DIGEST_SIZE> result1;
  std::copy(std::begin(result1_raw), std::end(result1_raw), result1.begin());

  // Test GetDigest with span
  auto result2 = SHA1Digest::GetDigest(test_span);

  EXPECT_EQ(result1, result2);
}

TEST(SHA1Digest, DigestToString)
{
  // Test the DigestToString method
  const std::string test_string = "abc";
  auto digest = SHA1Digest::GetDigest(test_string.data(), test_string.size());

  std::span<const u8, SHA1Digest::DIGEST_SIZE> digest_span(digest);
  std::string hex_string = SHA1Digest::DigestToString(digest_span);

  // Expected: a9993e364706816aba3e25717850c26c9cd0d89d
  EXPECT_EQ(hex_string, "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(SHA1Digest, BinaryData)
{
  // Test with binary data containing null bytes
  const u8 binary_data[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                            0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};

  auto result1 = SHA1Digest::GetDigest(binary_data, sizeof(binary_data));

  SHA1Digest digest;
  digest.Update(binary_data, sizeof(binary_data));
  u8 result2_raw[SHA1Digest::DIGEST_SIZE];
  digest.Final(result2_raw);

  std::array<u8, SHA1Digest::DIGEST_SIZE> result2;
  std::copy(std::begin(result2_raw), std::end(result2_raw), result2.begin());

  EXPECT_EQ(result1, result2);
}

TEST(MD5Digest, BinaryData)
{
  // Test MD5 with binary data containing null bytes
  const u8 binary_data[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                            0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};

  auto result1 = MD5Digest::HashData(std::span<const u8>(binary_data, sizeof(binary_data)));

  MD5Digest digest;
  digest.Update(binary_data, sizeof(binary_data));
  std::array<u8, MD5Digest::DIGEST_SIZE> result2;
  digest.Final(result2);

  EXPECT_EQ(result1, result2);
}
