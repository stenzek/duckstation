import sys
import argparse
import xml.etree.ElementTree as ET


def convert_list(filename, separator=','):
    fields = ["Game Code", "Game Title", "Region", "Compatibility", "Upscaling Issues", "Version tested", "Comments"]
    output = separator.join(fields) + "\n"

    tree = ET.parse(filename)
    for child in tree.getroot():
        if (child.tag != "entry"):
            print("!!! Skipping invalid tag '%s'" % child.tag)
            continue

        game_code = child.get("code")
        if game_code is None:
            game_code = ""
        game_title = child.get("title") or ""
        if game_title is None:
            game_title = ""
        region = child.get("region")
        if region is None:
            region = ""

        node = child.find("compatibility")
        compatibility = node.text if node is not None else ""
        node = child.find("upscaling-issues")
        upscaling_issues = node.text if node is not None else ""
        node = child.find("version-tested")
        version_tested = node.text if node is not None else ""
        node = child.find("comments")
        comments = node.text if node is not None else ""

        fix = None
        if separator == '\t':
            fix = lambda x: "" if x is None else x.replace('\t', '  ')
        elif separator == ',':
            fix = lambda x: "" if x is None else x if x.find(',') < 0 else ("\"%s\"" % x)
        else:
            fix = lambda x: "" if x is None else x

        entry_fields = [fix(game_code), fix(game_title), fix(region), fix(compatibility), fix(upscaling_issues), fix(version_tested), fix(comments)]
        output += separator.join(entry_fields) + "\n"

    return output



if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tabs", action="store_true")
    parser.add_argument("list_file", action="store")
    parser.add_argument("output_file", action="store")
    args = parser.parse_args()

    output = convert_list(args.list_file, '\t' if args.tabs else ',')
    output_file = open(args.output_file, "w")
    output_file.write(output)
    output_file.close()


