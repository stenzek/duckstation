import sys
import re
import xml.etree.ElementTree as ET
from xml.sax.saxutils import unescape

class GameEntry:
    def __init__(self, title, serials):
        self.title = title
        self.serials = serials

    def __repr__(self):
        return self.title + " (" + ",".join(self.serials) + ")"


def get_serials(s):
    out = []
    for it in s.split("/"):
        for it2 in it.split(","):
            for it3 in it2.split("~"):
                i = it3.find('(')
                if i > 0:
                    it3 = it3[:i-1]
                it3 = re.sub("[^A-Za-z0-9-]", "", it3)
                out.append(it3.strip())
    print(out)
    return out


def parse_xml(path):
    entries = {}
    tree = ET.parse(path)
    for child in tree.getroot():
        name = child.get("name")
        if name is None:
            continue

        title = ""
        description = child.find("description")
        if description is not None:
            title = description.text

        serials = []
        for grandchild in child.iterfind("info"):
            gname = grandchild.get("name")
            gvalue = grandchild.get("value")
            #print(gname, gvalue)
            if gname is not None and gname == "serial" and gvalue is not None:
                serials.extend(get_serials(gvalue))

        if len(serials) > 0:
            entries[name] = GameEntry(title, serials)

    return entries


def write_codes(entries, fout, name, codes):
    if name == "" or len(codes) == 0:
        return

    if name not in entries:
        print("Unknown game '%s'" % name)
        return

    entry = entries[name]
    fout.write(";%s\n" % entry.title)
    for serial in entry.serials:
        fout.write(":%s\n" % serial)
    fout.write("\n".join(codes))
    fout.write("\n\n")


def rewrite_dat(entries, inpath, outpath):
    fin = open(inpath, "r", encoding="utf-8")
    fout = open(outpath, "w", encoding="utf-8")

    current_name = ""
    code_lines = []
    
    for line in fin.readlines():
        if line[0] == ' ' or line[0] == ';' or line[:2] == "##":
            continue

        line = line.strip()
        if len(line) == 0:
            continue

        line = unescape(line)

        if line[0] == ':':
            write_codes(entries, fout, current_name, code_lines)
            current_name = line[1:].split(':')[0].strip()
            code_lines = []
        else:
            code_lines.append(line)

    write_codes(entries, fout, current_name, code_lines)

    fin.close()
    fout.close()


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("usage: %s <psx.xml path> <cheatpsx.dat> <output file>" % sys.argv[0])
        sys.exit(1)

    entries = parse_xml(sys.argv[1])
    rewrite_dat(entries, sys.argv[2], sys.argv[3])