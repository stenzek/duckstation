#!/usr/bin/env python3

import code
import sys
import os
import glob
import re

#src_file = "src/duckstation-qt/qttranslations.cpp"
src_dir = os.path.join(os.path.dirname(__file__), "..", "src")
fa_file = os.path.join(os.path.dirname(__file__), "..", "dep", "imgui", "include", "IconsFontAwesome5.h")
pf_file = os.path.join(os.path.dirname(__file__), "..", "dep", "imgui", "include", "IconsPromptFont.h")
emoji_file = os.path.join(os.path.dirname(__file__), "..", "dep", "imgui", "include", "IconsEmoji.h")
dst_file = os.path.join(os.path.dirname(__file__), "..", "src", "util", "imgui_glyph_ranges.inl")

all_source_files = glob.glob(os.path.join(src_dir, "**", "*.cpp"), recursive=True) + \
    glob.glob(os.path.join(src_dir, "**", "*.h"), recursive=True) + \
    glob.glob(os.path.join(src_dir, "**", "*.inl"), recursive=True)

tokens = set()
pf_tokens = set()
emoji_tokens = set()
for filename in all_source_files:
    data = None
    with open(filename, "r") as f:
        try:
            data = f.read()
        except:
            continue
    
    tokens = tokens.union(set(re.findall("(ICON_FA_[a-zA-Z0-9_]+)", data)))
    pf_tokens = pf_tokens.union(set(re.findall("(ICON_PF_[a-zA-Z0-9_]+)", data)))
    emoji_tokens = emoji_tokens.union(set(re.findall("(ICON_EMOJI_[a-zA-Z0-9_]+)", data)))

print("{}/{}/{} tokens found.".format(len(tokens), len(pf_tokens), len(emoji_tokens)))
if len(tokens) == 0 and len(pf_tokens) == 0:
    sys.exit(0)

u8_encodings = {}
with open(fa_file, "r") as f:
    for line in f.readlines():
        match = re.match("#define (ICON_FA_[^ ]+) \"([^\"]+)\"", line)
        if match is None:
            continue
        u8_encodings[match[1]] = bytes.fromhex(match[2].replace("\\x", ""))
with open(pf_file, "r") as f:
    for line in f.readlines():
        match = re.match("#define (ICON_PF_[^ ]+) \"([^\"]+)\"", line)
        if match is None:
            continue
        u8_encodings[match[1]] = bytes.fromhex(match[2].replace("\\x", ""))
with open(emoji_file, "r") as f:
    for line in f.readlines():
        match = re.match("#define (ICON_EMOJI_[^ ]+) \"([^\"]+)\"", line)
        if match is None:
            continue
        u8_encodings[match[1]] = bytes.fromhex(match[2].replace("\\x", ""))

out_pattern = "(static constexpr ImWchar FA_ICON_RANGE\\[\\] = \\{)[0-9A-Z_a-z, \n]+(\\};)"
out_pf_pattern = "(static constexpr ImWchar PF_ICON_RANGE\\[\\] = \\{)[0-9A-Z_a-z, \n]+(\\};)"
out_emoji_pattern = "(static constexpr ImWchar EMOJI_ICON_RANGE\\[\\] = \\{)[0-9A-Z_a-z, \n]+(\\};)"

def get_pairs(tokens):
    codepoints = list()
    for token in tokens:
        u8_bytes = u8_encodings[token]
        u8 = str(u8_bytes, "utf-8")
        u32 = u8.encode("utf-32le")
        if len(u32) > 4:
            raise ValueError("{} {} too long".format(u8_bytes, token))

        codepoint = int.from_bytes(u32, byteorder="little", signed=False)
        codepoints.append(codepoint)
    codepoints.sort()
    codepoints.append(0) # null terminator

    startc = codepoints[0]
    endc = None
    pairs = [startc]
    for codepoint in codepoints:
        if endc is not None and (endc + 1) != codepoint:
            pairs.append(endc)
            pairs.append(codepoint)
            startc = codepoint
            endc = codepoint
        else:
            endc = codepoint
    pairs.append(endc)

    pairs_str = ",".join(list(map(lambda x: "0x{:x}".format(x), pairs)))
    return pairs_str

with open(dst_file, "r") as f:
    original = f.read()
    updated = re.sub(out_pattern, "\\1 " + get_pairs(tokens) + " \\2", original)
    updated = re.sub(out_pf_pattern, "\\1 " + get_pairs(pf_tokens) + " \\2", updated)
    updated = re.sub(out_emoji_pattern, "\\1 " + get_pairs(emoji_tokens) + " \\2", updated)
    if original != updated:
        with open(dst_file, "w") as f:
            f.write(updated)
            print("Updated {}".format(dst_file))
    else:
        print("Skipping updating {}".format(dst_file))

