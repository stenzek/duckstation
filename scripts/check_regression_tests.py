#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0

import argparse
import glob
import json
import sys
import os
import re
import hashlib

from pathlib import Path

HTML_TEMPLATE_PATH = os.path.join(os.path.dirname(os.path.realpath(__file__)), "check_regression_tests.html")

MAX_DIFF_FRAMES = 0

results = []


def compare_frames(path1, path2):
    try:
        if os.stat(path1).st_size != os.stat(path2).st_size:
            return False

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
        print("*** %s is missing in test set" % name)
        results.append({"name": name, "missingDir": True, "diffFrames": [], "missingFrames": [], "frames": []})
        return False

    images = []
    for imagepath in glob.glob(os.path.join(dir1, "frame_*.png")):
        matches = re.match("frame_([0-9]+).png", Path(imagepath).name)
        if matches is not None:
            images.append((int(matches[1]), Path(imagepath).name))
    images.sort()

    diff_frames = []
    missing_frames = []

    for (framenum, imagename) in images:
        path1 = os.path.join(dir1, imagename)
        path2 = os.path.join(dir2, imagename)
        if not os.path.isfile(path2):
            print("--- Frame %u for %s is missing in test set" % (framenum, name))
            missing_frames.append(framenum)
            continue

        if not compare_frames(path1, path2):
            diff_frames.append((framenum, path1, path2))

    if diff_frames or missing_frames:
        results.append({
            "name": name,
            "missingDir": False,
            "diffFrames": [x[0] for x in diff_frames],
            "missingFrames": missing_frames,
            "frames": [{"n": framenum, "before": Path(path1).as_uri(), "after": Path(path2).as_uri()}
                       for (framenum, path1, path2) in (diff_frames[-MAX_DIFF_FRAMES:] if MAX_DIFF_FRAMES > 0 else diff_frames)],
        })

    if diff_frames:
        print("*** Difference in frames [%s] for %s" % (",".join(map(lambda x: str(x[0]), diff_frames)), name))

    return not diff_frames and not missing_frames


def check_regression_tests(baselinedir, testdir):
    gamedirs = [x for x in glob.glob(baselinedir + "/*", recursive=False) if os.path.isdir(x)]
    gamedirs.sort(key=lambda x: os.path.basename(x))
    if not gamedirs:
        print("*** No games found in baseline directory %s" % baselinedir)
        return None

    success = 0
    failure = 0

    for gamedir in gamedirs:
        name = Path(gamedir).name
        if check_regression_test(baselinedir, testdir, name):
            success += 1
        else:
            failure += 1

    return (success, failure)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Check frame dump images for regression tests")
    parser.add_argument("-baselinedir", action="store", required=True, help="Directory containing baseline frames to check against")
    parser.add_argument("-testdir", action="store", required=True, help="Directory containing frames to check")
    parser.add_argument("-maxframes", type=int, action="store", required=False, default=0, help="Max differing frames to show per game in the report, 0 for unlimited")
    parser.add_argument("outfile", action="store", help="The file to write the output to")

    args = parser.parse_args()
    MAX_DIFF_FRAMES = args.maxframes
    if MAX_DIFF_FRAMES < 0:
        parser.error("-maxframes must not be negative")

    baselinedir = os.path.realpath(args.baselinedir)
    testdir = os.path.realpath(args.testdir)
    counts = check_regression_tests(baselinedir, testdir)
    if counts is None:
        sys.exit(1)

    (success, failure) = counts
    if failure > 0:
        data = {"baseline": baselinedir, "test": testdir, "passed": success, "games": results}
        with open(HTML_TEMPLATE_PATH, "r") as f:
            template = f.read()
        with open(args.outfile, "w") as f:
            f.write(template.replace("__DATA__", json.dumps(data).replace("</", "<\\/")))
        sys.exit(1)
    else:
        if os.path.exists(args.outfile):
            os.remove(args.outfile)
        sys.exit(0)
