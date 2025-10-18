import code
import sys
import os
import glob
import re

START_IDENT = "// TRANSLATION-STRING-AREA-BEGIN"
END_IDENT = "// TRANSLATION-STRING-AREA-END"
SRC_FILES = ["src/core/fullscreenui.cpp",
             "src/core/fullscreenui.h",
             "src/core/fullscreenui_game_list.cpp",
             "src/core/fullscreenui_private.h",
             "src/core/fullscreenui_settings.cpp",
             "src/core/fullscreenui_widgets.cpp",
             "src/core/fullscreenui_widgets.h"]
DST_FILE = "src/core/fullscreenui.cpp"

full_source = ""
for src_file in SRC_FILES:
    path = os.path.join(os.path.dirname(__file__), "..", src_file)
    with open(path, "r") as f:
        full_source += f.read()
        full_source += "\n"

strings = set()
for token in ["FSUI_STR", "FSUI_CSTR", "FSUI_FSTR", "FSUI_NSTR", "FSUI_VSTR", "FSUI_ICONSTR", "FSUI_ICONVSTR", "FSUI_ICONCSTR"]:
    token_len = len(token)
    last_pos = 0
    while True:
        last_pos = full_source.find(token, last_pos)
        if last_pos < 0:
            break

        if last_pos >= 8 and full_source[last_pos - 8:last_pos] == "#define ":
            last_pos += len(token)
            continue

        if full_source[last_pos + token_len] == '(':
            start_pos = last_pos + token_len + 1
            end_pos = full_source.find("\")", start_pos)
            s = full_source[start_pos:end_pos+1]

            # remove "
            pos = s.find('"')
            new_s = ""
            while pos >= 0:
                if pos == 0 or s[pos - 1] != '\\':
                    epos = pos
                    while True:
                        epos = s.find('"', epos + 1)
                        assert epos > pos
                        if s[epos - 1] == '\\':
                            continue
                        else:
                            break

                    assert epos > pos
                    new_s += s[pos+1:epos]
                    pos = s.find('"', epos + 1)
                else:
                    pos = s.find('"', pos + 1)
            assert len(new_s) > 0

            assert (end_pos - start_pos) < 300
            strings.add(new_s)
        last_pos += len(token)

print(f"Found {len(strings)} unique strings.")

full_source = ""
dst_path = os.path.join(os.path.dirname(__file__), "..", DST_FILE)
with open(dst_path, "r") as f:
    full_source = f.read()
start = full_source.find(START_IDENT)
end = full_source.find(END_IDENT)
assert start >= 0 and end > start

new_area = ""
for string in sorted(list(strings)):
    new_area += f"TRANSLATE_NOOP(\"FullscreenUI\", \"{string}\");\n"

full_source = full_source[:start+len(START_IDENT)+1] + new_area + full_source[end:]
with open(dst_path, "w") as f:
    f.write(full_source)
