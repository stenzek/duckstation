import code
import sys
import os
import glob
import re

#src_file = "src/duckstation-qt/qttranslations.cpp"
src_dir = os.path.join(os.path.dirname(__file__), "..", "src")
fa_file = os.path.join(os.path.dirname(__file__), "..", "dep", "imgui", "include", "IconsFontAwesome5.h")
dst_file = os.path.join(os.path.dirname(__file__), "..", "src", "frontend-common", "imgui_manager.cpp")

all_source_files = glob.glob(os.path.join(src_dir, "**", "*.cpp"), recursive=True) + \
    glob.glob(os.path.join(src_dir, "**", "*.h"), recursive=True) + \
    glob.glob(os.path.join(src_dir, "**", "*.inl"), recursive=True)

tokens = set()
for filename in all_source_files:
    data = None
    with open(filename, "r") as f:
        try:
            data = f.read()
        except:
            continue
    
    tokens = tokens.union(set(re.findall("(ICON_FA_[a-zA-Z0-9_]+)", data)))

print("{} tokens found.".format(len(tokens)))
if len(tokens) == 0:
    sys.exit(0)

u8_encodings = {}
with open(fa_file, "r") as f:
    for line in f.readlines():
        match = re.match("#define (ICON_FA_[^ ]+) \"([^\"]+)\"", line)
        if match is None:
            continue
        u8_encodings[match[1]] = bytes.fromhex(match[2].replace("\\x", ""))

out_pattern = "(static constexpr ImWchar range_fa\[\] = \{)[0-9A-Z_a-z, \n]+(\};)"

codepoints = list()
for token in tokens:
    u8_bytes = u8_encodings[token]
    u8 = str(u8_bytes, "utf-8")
    u16 = u8.encode("utf-16le")
    if len(u16) > 2:
        raise ValueError("{} too long".format(u8_bytes))

    codepoint = int.from_bytes(u16, byteorder="little", signed=False)
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

with open(dst_file, "r") as f:
    original = f.read()
    updated = re.sub(out_pattern, "\\1 " + pairs_str + " \\2", original)
    if original != updated:
        with open(dst_file, "w") as f:
            f.write(updated)
            print("Updated {}".format(dst_file))
    else:
        print("Skipping updating {}".format(dst_file))

