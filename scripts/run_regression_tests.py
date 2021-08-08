import argparse
import glob
import sys
import os
import subprocess
import multiprocessing
from functools import partial

def is_game_path(path):
    idx = path.rfind('.')
    if idx < 0:
        return False

    extension = path[idx + 1:].strip().lower()
    return extension in ["cue", "chd"]


def run_regression_test(runner, destdir, dump_interval, frames, gamepath):
    args = [runner,
            "-renderer", "software",
            "-log", "verbose",
            "-dumpdir", destdir,
            "-dumpinterval", str(dump_interval),
            "-frames", str(frames),
            "--", gamepath
    ]

    print("Running '%s'" % (" ".join(args)))
    subprocess.run(args)


def run_regression_tests(runner, gamedir, destdir, dump_interval, frames, parallel=1):
    paths = glob.glob(gamedir + "/*.*", recursive=True)
    gamepaths = list(filter(is_game_path, paths))

    if not os.path.isdir(destdir) and not os.mkdir(destdir):
        print("Failed to create directory")
        return False

    print("Found %u games" % len(gamepaths))

    if parallel <= 1:
        for game in gamepaths:
            run_regression_test(runner, destdir, dump_interval, frames, game)
    else:
        print("Processing %u games on %u processors" % (len(gamepaths), parallel))
        func = partial(run_regression_test, runner, destdir, dump_interval, frames)
        pool = multiprocessing.Pool(parallel)
        pool.map(func, gamepaths)
        pool.close()


    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate frame dump images for regression tests")
    parser.add_argument("-runner", action="store", required=True, help="Path to DuckStation regression test runner")
    parser.add_argument("-gamedir", action="store", required=True, help="Directory containing game images")
    parser.add_argument("-destdir", action="store", required=True, help="Base directory to dump frames to")
    parser.add_argument("-dumpinterval", action="store", type=int, required=True, help="Interval to dump frames at")
    parser.add_argument("-frames", action="store", type=int, default=3600, help="Number of frames to run")
    parser.add_argument("-parallel", action="store", type=int, default=1, help="Number of proceeses to run")

    args = parser.parse_args()

    if not run_regression_tests(args.runner, os.path.realpath(args.gamedir), os.path.realpath(args.destdir), args.dumpinterval, args.frames, args.parallel):
        sys.exit(1)
    else:
        sys.exit(0)

