from copy import deepcopy
import sys
import argparse
import xml.etree.ElementTree as ET
from xml.dom import minidom

# https://pymotw.com/2/xml/etree/ElementTree/create.html
def prettify(elem):
    """Return a pretty-printed XML string for the Element.
    """
    rough_string = ET.tostring(elem, 'utf-8')
    reparsed = minidom.parseString(rough_string)
    dom_string = reparsed.toprettyxml(encoding="utf-8",indent="  ")
    return b'\n'.join([s for s in dom_string.splitlines() if s.strip()])


# https://stackoverflow.com/questions/25338817/sorting-xml-in-python-etree/25339725#25339725
def sortchildrenby(parent, attr):
    parent[:] = sorted(parent, key=lambda child: child.get(attr))


def add_entries_from_file(filename, new_tree, overwrite_existing = False):
    tree = ET.parse(filename)
    for child in tree.getroot():
        if (child.tag != "entry"):
            print("!!! Skipping invalid tag '%s'" % child.tag)
            continue

        game_code = child.get("code")
        existing_node = new_tree.getroot().find(".//*[@code='%s']" % game_code)
        if existing_node is not None:
            if overwrite_existing:
                print("*** Replacing %s from new list" % game_code)
                new_tree.getroot().remove(existing_node)
            else:
                print("*** Skipping %s from new list" % game_code)
                continue

        new_tree.getroot().append(deepcopy(child))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("existing_list", action="store")
    parser.add_argument("list_to_merge", action="store")
    parser.add_argument("output_list", action="store")
    args = parser.parse_args()

    new_tree = ET.ElementTree(ET.Element("compatibility-list"))
    add_entries_from_file(args.existing_list, new_tree, False)
    add_entries_from_file(args.list_to_merge, new_tree, args.overwrite)

    sortchildrenby(new_tree.getroot(), "title")

    output_file = open(args.output_list, "wb")
    output_file.write(prettify(new_tree.getroot()))
    output_file.close()

