// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "util/cue_parser.h"

#include "common/error.h"

#include <gtest/gtest.h>

// Test basic valid CUE sheet
TEST(CueParser, BasicValidCue)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 MODE2/2352\n"
                          "INDEX 01 00:00:00\n";

  CueParser::File cue_file;
  Error error;

  ASSERT_TRUE(cue_file.Parse(cue, &error));

  const CueParser::Track* track = cue_file.GetTrack(1);
  ASSERT_NE(track, nullptr);
  EXPECT_EQ(track->number, 1);
  EXPECT_EQ(track->mode, CueParser::TrackMode::Mode2Raw);
  EXPECT_EQ(track->file, "game.bin");
  EXPECT_EQ(track->file_format, CueParser::FileFormat::Binary);

  const CueParser::MSF* index1 = track->GetIndex(1);
  ASSERT_NE(index1, nullptr);
  EXPECT_EQ(index1->minute, 0);
  EXPECT_EQ(index1->second, 0);
  EXPECT_EQ(index1->frame, 0);
}

// Test multiple tracks
TEST(CueParser, MultipleTracksCue)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 MODE1/2352\n"
                          "INDEX 01 00:00:00\n"
                          "TRACK 02 AUDIO\n"
                          "INDEX 00 02:30:65\n"
                          "INDEX 01 02:31:45\n"
                          "TRACK 03 MODE2/2336\n"
                          "INDEX 01 05:45:20\n";

  CueParser::File cue_file;
  Error error;

  ASSERT_TRUE(cue_file.Parse(cue, &error));

  // Test track 1
  const CueParser::Track* track1 = cue_file.GetTrack(1);
  ASSERT_NE(track1, nullptr);
  EXPECT_EQ(track1->number, 1);
  EXPECT_EQ(track1->mode, CueParser::TrackMode::Mode1Raw);

  // Test track 2
  const CueParser::Track* track2 = cue_file.GetTrack(2);
  ASSERT_NE(track2, nullptr);
  EXPECT_EQ(track2->number, 2);
  EXPECT_EQ(track2->mode, CueParser::TrackMode::Audio);

  // Check track 2 indices
  const CueParser::MSF* index0 = track2->GetIndex(0);
  ASSERT_NE(index0, nullptr);
  EXPECT_EQ(index0->minute, 2);
  EXPECT_EQ(index0->second, 30);
  EXPECT_EQ(index0->frame, 65);

  const CueParser::MSF* index1 = track2->GetIndex(1);
  ASSERT_NE(index1, nullptr);
  EXPECT_EQ(index1->minute, 2);
  EXPECT_EQ(index1->second, 31);
  EXPECT_EQ(index1->frame, 45);

  // Test track 3
  const CueParser::Track* track3 = cue_file.GetTrack(3);
  ASSERT_NE(track3, nullptr);
  EXPECT_EQ(track3->number, 3);
  EXPECT_EQ(track3->mode, CueParser::TrackMode::Mode2);
}

// Test different track modes
TEST(CueParser, DifferentTrackModes)
{
  const std::string cue = "FILE \"audio.wav\" WAVE\n"
                          "TRACK 01 AUDIO\n"
                          "INDEX 01 00:00:00\n"
                          "FILE \"data.bin\" BINARY\n"
                          "TRACK 02 MODE1/2048\n"
                          "INDEX 01 00:00:00\n"
                          "TRACK 03 MODE1/2352\n"
                          "INDEX 01 15:34:50\n"
                          "TRACK 04 MODE2/2048\n"
                          "INDEX 01 30:10:00\n"
                          "TRACK 05 MODE2/2352\n"
                          "INDEX 01 45:00:00\n";

  CueParser::File cue_file;
  Error error;

  ASSERT_TRUE(cue_file.Parse(cue, &error));

  // Verify track modes
  const CueParser::Track* track1 = cue_file.GetTrack(1);
  ASSERT_NE(track1, nullptr);
  EXPECT_EQ(track1->mode, CueParser::TrackMode::Audio);
  EXPECT_EQ(track1->file_format, CueParser::FileFormat::Wave);

  const CueParser::Track* track2 = cue_file.GetTrack(2);
  ASSERT_NE(track2, nullptr);
  EXPECT_EQ(track2->mode, CueParser::TrackMode::Mode1);
  EXPECT_EQ(track2->file_format, CueParser::FileFormat::Binary);

  const CueParser::Track* track3 = cue_file.GetTrack(3);
  ASSERT_NE(track3, nullptr);
  EXPECT_EQ(track3->mode, CueParser::TrackMode::Mode1Raw);

  const CueParser::Track* track4 = cue_file.GetTrack(4);
  ASSERT_NE(track4, nullptr);
  EXPECT_EQ(track4->mode, CueParser::TrackMode::Mode2Form1);

  const CueParser::Track* track5 = cue_file.GetTrack(5);
  ASSERT_NE(track5, nullptr);
  EXPECT_EQ(track5->mode, CueParser::TrackMode::Mode2Raw);
}

// Test track flags
TEST(CueParser, TrackFlags)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 AUDIO\n"
                          "FLAGS PRE DCP 4CH\n"
                          "INDEX 01 00:00:00\n"
                          "TRACK 02 AUDIO\n"
                          "FLAGS SCMS\n"
                          "INDEX 01 03:00:00\n";

  CueParser::File cue_file;
  Error error;

  ASSERT_TRUE(cue_file.Parse(cue, &error));

  const CueParser::Track* track1 = cue_file.GetTrack(1);
  ASSERT_NE(track1, nullptr);
  EXPECT_TRUE(track1->HasFlag(CueParser::TrackFlag::PreEmphasis));
  EXPECT_TRUE(track1->HasFlag(CueParser::TrackFlag::CopyPermitted));
  EXPECT_TRUE(track1->HasFlag(CueParser::TrackFlag::FourChannelAudio));
  EXPECT_FALSE(track1->HasFlag(CueParser::TrackFlag::SerialCopyManagement));

  const CueParser::Track* track2 = cue_file.GetTrack(2);
  ASSERT_NE(track2, nullptr);
  EXPECT_FALSE(track2->HasFlag(CueParser::TrackFlag::PreEmphasis));
  EXPECT_FALSE(track2->HasFlag(CueParser::TrackFlag::CopyPermitted));
  EXPECT_FALSE(track2->HasFlag(CueParser::TrackFlag::FourChannelAudio));
  EXPECT_TRUE(track2->HasFlag(CueParser::TrackFlag::SerialCopyManagement));
}

// Test PREGAP handling
TEST(CueParser, PregapHandling)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 AUDIO\n"
                          "INDEX 01 00:00:00\n"
                          "TRACK 02 AUDIO\n"
                          "PREGAP 00:02:00\n"
                          "INDEX 01 03:00:00\n";

  CueParser::File cue_file;
  Error error;

  ASSERT_TRUE(cue_file.Parse(cue, &error));

  const CueParser::Track* track1 = cue_file.GetTrack(1);
  ASSERT_NE(track1, nullptr);
  EXPECT_FALSE(track1->zero_pregap.has_value());

  const CueParser::Track* track2 = cue_file.GetTrack(2);
  ASSERT_NE(track2, nullptr);
  ASSERT_TRUE(track2->zero_pregap.has_value());
  EXPECT_EQ(track2->zero_pregap->minute, 0);
  EXPECT_EQ(track2->zero_pregap->second, 2);
  EXPECT_EQ(track2->zero_pregap->frame, 0);
}

// Test track lengths are correctly calculated
TEST(CueParser, TrackLengthCalculation)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 AUDIO\n"
                          "INDEX 01 00:00:00\n"
                          "TRACK 02 AUDIO\n"
                          "INDEX 00 02:30:00\n"
                          "INDEX 01 02:32:00\n"
                          "TRACK 03 AUDIO\n"
                          "INDEX 01 05:45:20\n";

  CueParser::File cue_file;
  Error error;

  ASSERT_TRUE(cue_file.Parse(cue, &error));

  const CueParser::Track* track1 = cue_file.GetTrack(1);
  ASSERT_NE(track1, nullptr);
  ASSERT_TRUE(track1->length.has_value());
  // Track 1 length should be calculated to end at track 2's index 0
  EXPECT_EQ(track1->length->minute, 2);
  EXPECT_EQ(track1->length->second, 30);
  EXPECT_EQ(track1->length->frame, 0);

  const CueParser::Track* track2 = cue_file.GetTrack(2);
  ASSERT_NE(track2, nullptr);
  ASSERT_TRUE(track2->length.has_value());
  // Track 2 length should be calculated to end at track 3's index 1
  EXPECT_EQ(track2->length->minute, 3);
  EXPECT_EQ(track2->length->second, 13);
  EXPECT_EQ(track2->length->frame, 20);
}

// Test missing FILE command
TEST(CueParser, MissingFileCommand)
{
  const std::string cue = "TRACK 01 AUDIO\n"
                          "INDEX 01 00:00:00\n";

  CueParser::File cue_file;
  Error error;
  ASSERT_FALSE(cue_file.Parse(cue, &error));
}

// Test invalid track number
TEST(CueParser, InvalidTrackNumber)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 100 AUDIO\n"
                          "INDEX 01 00:00:00\n";

  CueParser::File cue_file;
  Error error;
  ASSERT_FALSE(cue_file.Parse(cue, &error));
}

// Test missing index 1
TEST(CueParser, MissingIndex1)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 AUDIO\n"
                          "INDEX 00 00:00:00\n";

  CueParser::File cue_file;
  Error error;
  ASSERT_FALSE(cue_file.Parse(cue, &error));
}

// Test invalid index number
TEST(CueParser, InvalidIndexNumber)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 AUDIO\n"
                          "INDEX 100 00:00:00\n";

  CueParser::File cue_file;
  Error error;
  ASSERT_FALSE(cue_file.Parse(cue, &error));
}

// Test invalid MSF format
TEST(CueParser, InvalidMSFFormat)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 AUDIO\n"
                          "INDEX 01 00:99:00\n";

  CueParser::File cue_file;
  Error error;
  ASSERT_FALSE(cue_file.Parse(cue, &error));
}

// Test duplicate index
TEST(CueParser, DuplicateIndex)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 AUDIO\n"
                          "INDEX 01 00:00:00\n"
                          "INDEX 01 00:01:00\n";

  CueParser::File cue_file;
  Error error;
  ASSERT_FALSE(cue_file.Parse(cue, &error));
}

// Test reverse index order
TEST(CueParser, ReverseIndexOrder)
{
  const std::string cue = "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 AUDIO\n"
                          "INDEX 02 00:01:00\n"
                          "INDEX 01 00:02:00\n";

  CueParser::File cue_file;
  Error error;
  ASSERT_FALSE(cue_file.Parse(cue, &error));
}

// Test handling of comments and whitespace
TEST(CueParser, CommentsAndWhitespace)
{
  const std::string cue = "REM This is a comment\n"
                          "   FILE    \"game.bin\"    BINARY   \n"
                          "\n"
                          "REM Another comment\n"
                          "  TRACK  01  MODE2/2352  \n"
                          "    INDEX    01    00:00:00    \n";

  CueParser::File cue_file;
  Error error;

  ASSERT_TRUE(cue_file.Parse(cue, &error));

  const CueParser::Track* track = cue_file.GetTrack(1);
  ASSERT_NE(track, nullptr);
  EXPECT_EQ(track->mode, CueParser::TrackMode::Mode2Raw);
}

// Test handling of multiple files
TEST(CueParser, MultipleFiles)
{
  const std::string cue = "FILE \"track1.bin\" BINARY\n"
                          "TRACK 01 MODE1/2352\n"
                          "INDEX 01 00:00:00\n"
                          "FILE \"track2.wav\" WAVE\n"
                          "TRACK 02 AUDIO\n"
                          "INDEX 01 00:00:00\n";

  CueParser::File cue_file;
  Error error;

  ASSERT_TRUE(cue_file.Parse(cue, &error));

  const CueParser::Track* track1 = cue_file.GetTrack(1);
  ASSERT_NE(track1, nullptr);
  EXPECT_EQ(track1->file, "track1.bin");
  EXPECT_EQ(track1->file_format, CueParser::FileFormat::Binary);

  const CueParser::Track* track2 = cue_file.GetTrack(2);
  ASSERT_NE(track2, nullptr);
  EXPECT_EQ(track2->file, "track2.wav");
  EXPECT_EQ(track2->file_format, CueParser::FileFormat::Wave);
}

// Test ignoring unsupported metadata
TEST(CueParser, IgnoreUnsupportedMetadata)
{
  const std::string cue = "CATALOG 1234567890123\n"
                          "TITLE \"Game Title\"\n"
                          "PERFORMER \"Developer\"\n"
                          "FILE \"game.bin\" BINARY\n"
                          "TRACK 01 MODE2/2352\n"
                          "TITLE \"Track Title\"\n"
                          "INDEX 01 00:00:00\n";

  CueParser::File cue_file;
  Error error;

  ASSERT_TRUE(cue_file.Parse(cue, &error));

  const CueParser::Track* track = cue_file.GetTrack(1);
  ASSERT_NE(track, nullptr);
  EXPECT_EQ(track->number, 1);
}
