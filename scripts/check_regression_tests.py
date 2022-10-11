import argparse
import glob
import sys
import os
import re
import hashlib

from pathlib import Path

FILE_HEADER = """
<!DOCTYPE html>
<html>
<head>
<title>Comparison</title>
</head>
<body>
"""

FILE_FOOTER = """
</body>
</html>
"""

outfile = None
def write(line):
    outfile.write(line + "\n")


def compare_frames(path1, path2):
    try:
        with open(path1, "rb") as f:
            hash1 = hashlib.md5(f.read()).digest()
        with open(path2, "rb") as f:
            hash2 = hashlib.md5(f.read()).digest()

        #print(hash1, hash2)
        return hash1 == hash2
    except:
        return False


def check_regression_test(baselinedir, testdir, name):
    #print("Checking '%s'..." % name)

    dir1 = os.path.join(baselinedir, name)
    dir2 = os.path.join(testdir, name)
    if not os.path.isdir(dir2):
        #print("*** %s is missing in test set" % name)
        return False

    images = glob.glob(os.path.join(dir1, "frame_*.png"))
    for imagepath in images:
        imagename = Path(imagepath).name
        matches = re.match("frame_([0-9]+).png", imagename)
        if matches is None:
            continue

        framenum = int(matches[1])

        path1 = os.path.join(dir1, imagename)
        path2 = os.path.join(dir2, imagename)
        if not os.path.isfile(path2):
            print("--- Frame %u for %s is missing in test set" % (framenum, name))
            write("<h1>{}</h1>".format(name))
            write("--- Frame %u for %s is missing in test set" % (framenum, name))
            return False

        if not compare_frames(path1, path2):
            print("*** Difference in frame %u for %s" % (framenum, name))

            imguri1 = Path(path1).as_uri()
            imguri2 = Path(path2).as_uri()
            write("<h1>{}</h1>".format(name))
            write("<table width=\"100%\">")
            write("<tr><td colspan=\"2\">Frame {}</td></tr>".format(framenum))
            write("<tr><td><img src=\"{}\" /></td><td><img src=\"{}\" /></td></tr>".format(imguri1, imguri2))
            write("</table>")
            return False

    return True


def check_regression_tests(baselinedir, testdir):
    gamedirs = glob.glob(baselinedir + "/*", recursive=False)
    
    success = 0
    failure = 0

    for gamedir in gamedirs:
        name = Path(gamedir).name
        if check_regression_test(baselinedir, testdir, name):
            success += 1
        else:
            failure += 1

    return (failure == 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Check frame dump images for regression tests")
    parser.add_argument("-baselinedir", action="store", required=True, help="Directory containing baseline frames to check against")
    parser.add_argument("-testdir", action="store", required=True, help="Directory containing frames to check")
    parser.add_argument("outfile", action="store", help="The file to write the output to")

    args = parser.parse_args()

    outfile = open(args.outfile, "w")
    write(FILE_HEADER)

    if not check_regression_tests(os.path.realpath(args.baselinedir), os.path.realpath(args.testdir)):
        write(FILE_FOOTER)
        outfile.close()
        sys.exit(1)
    else:
        outfile.close()
        os.remove(args.outfile)
        sys.exit(0)

