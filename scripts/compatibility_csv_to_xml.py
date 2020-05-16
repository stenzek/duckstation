import csv
import sys
from xml.sax.saxutils import escape

SKIP_COLS = 1
SKIP_ROWS = 3

def compatibility_string_to_int(value):
    value_lower = value.lower()
    if (value_lower == "doesn't boot"):
        return 1
    elif (value_lower == "crashes in intro"):
        return 2
    elif (value_lower == "crashes in-game"):
        return 3
    elif (value_lower == "graphical/audio issues"):
        return 4
    elif (value_lower == "no issues"):
        return 5

    print("*** Unknown compatibility level string: '%s'" % value)
    return 0


def compatibility_csv_to_xml(input_file, output_file):
    fin = open(input_file, "r")
    if (not input_file):
        print("Failed to open %s" % input_file)
        return False

    fout = open(output_file, "w")
    if (not output_file):
        print("Failed to open %s" % output_file)
        return False

    fout.write("<?xml version=\"1.0\"?>\n")
    fout.write("<compatibility-list>\n")

    row_number = 0
    for row in csv.reader(fin):
        row_number += 1
        if (row_number <= SKIP_ROWS):
            continue
        # Skip header rows
        # TODO: Proper map for these if the column order changes
        #if (row[SKIP_COLS + 0] == "Game Code" or row[SKIP_COLS + 1] == "Game Title" or row[SKIP_COLS + 2] == "Region" or
        #    row[SKIP_COLS + 3] == "Compatibility" or row[SKIP_COLS + 4] == "Upscaling Issues" or
        #    row[SKIP_COLS + 5] == "Version tested" or row[SKIP_COLS + 6] == "Comments"):
        #       continue

        code = str(row[SKIP_COLS + 0]).strip()
        title = str(row[SKIP_COLS + 1]).strip()
        region = str(row[SKIP_COLS + 2]).strip()
        compatibility = str(row[SKIP_COLS + 3]).strip()
        upscaling_issues = str(row[SKIP_COLS + 4]).strip()
        version_tested = str(row[SKIP_COLS + 5]).strip()
        comments = str(row[SKIP_COLS + 6]).strip()

        if (len(code) == 0):
            print("** Code is missing for '%s' (%s), skipping" % (title, region))
            continue

        # TODO: Quoting here
        fout.write("  <entry code=\"%s\" title=\"%s\" region=\"%s\" compatibility=\"%d\">\n" % (escape(code), escape(title), escape(region), compatibility_string_to_int(compatibility)))
        fout.write("    <compatibility>%s</compatibility>\n" % escape(compatibility))
        if (len(upscaling_issues) > 0):
            fout.write("    <upscaling-issues>%s</upscaling-issues>\n" % escape(upscaling_issues))
        if (len(version_tested) > 0):
            fout.write("    <version-tested>%s</version-tested>\n" % escape(version_tested))
        if (len(comments) > 0):
            fout.write("    <comments>%s</comments>\n" % escape(comments))
        fout.write("  </entry>\n")

    fout.write("</compatibility-list>\n")
    fout.close()
    fin.close()
    return True


if (__name__ == "__main__"):
    if (len(sys.argv) < 3):
        print("Usage: %s <path to csv> <path to xml>" % sys.argv[0])
        sys.exit(1)

    result = compatibility_csv_to_xml(sys.argv[1], sys.argv[2])
    sys.exit(0 if result else 1)

