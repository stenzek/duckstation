import sys
import os
import re
import xml.etree.ElementTree as ET

src_file = "src/duckstation-qt/qttranslations.cpp"
root_dir = os.path.join(os.path.dirname(__file__), "..")
src_path = os.path.join(root_dir, src_file)

def parse_xml(path):
    translations = ""
    tree = ET.parse(path)
    root = tree.getroot()
    for node in root.findall("context/message/translation"):
        if node.text:
            translations += node.text

    chars = list(set([ord(ch) for ch in translations if ord(ch) >= 0x2000]))
    chars.sort()
    chars = "".join([chr(ch) for ch in chars])
    return chars

def update_src_file(ts_file, chars):
    ts_name = os.path.basename(ts_file)
    pattern = re.compile(u'(// auto update.*' + ts_name + '.*\n[^"]+")[^"]*(".*)')
    with open(src_path) as f:
        original = f.read()
        update = pattern.sub(u'\\1' + chars + '\\2', original)
    if original != update:
        with open(src_path, 'w') as f:
            f.write(update)
        print("updated " + src_file)
    else:
        print("no need to update " + src_file)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: %s <duckstation-qt_*.ts path>" % sys.argv[0])
        sys.exit(1)

    chars = parse_xml(sys.argv[1])
    print (chars)
    print ("%d character(s) detected." % len(chars))
    update_src_file(sys.argv[1], chars)
