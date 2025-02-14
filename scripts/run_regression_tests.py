import argparse
import glob
import sys
import os
import subprocess
import multiprocessing
from functools import partial

def is_game_path(path:str):
    lpath = path.lower()
    for extension in ["cue", "chd", "psxgpu", "psxgpu.zst", "psxgpu.xz"]:
        if path.endswith(extension):
            return True
    return False


def run_regression_test(runner, destdir, dump_interval, frames, renderer, cargs, gamepath):
    args = [runner,
            "-log", "error",
            "-dumpdir", destdir,
            "-dumpinterval", str(dump_interval),
            "-frames", str(frames),
            "-renderer", ("Software" if renderer is None else renderer),
    ]
    args += cargs
    args += ["--", gamepath]

    #print("Running '%s'" % (" ".join(args)))
    subprocess.run(args)
    return os.path.basename(gamepath)


def run_regression_tests(runner, gamedirs, destdir, dump_interval, frames, parallel, renderer, cargs):
    paths = []
    for gamedir in gamedirs:
        paths += glob.glob(os.path.realpath(gamedir) + "/*.*", recursive=True)
    gamepaths = list(filter(is_game_path, paths))
    gamepaths.sort(key=lambda x: os.path.basename(x))

    try:
        if not os.path.isdir(destdir):
            os.mkdir(destdir)
    except OSError:
        print("Failed to create directory")
        return False

    print("Found %u games" % len(gamepaths))

    if parallel <= 1:
        for game in gamepaths:
            run_regression_test(runner, destdir, dump_interval, frames, renderer, cargs, game)
    else:
        print("Processing %u games on %u processors" % (len(gamepaths), parallel))
        func = partial(run_regression_test, runner, destdir, dump_interval, frames, renderer, cargs)
        pool = multiprocessing.Pool(parallel)
        completed = 0
        for filename in pool.imap_unordered(func, gamepaths, chunksize=1):
            completed += 1
            print("[%u%% %u/%u] %s" % ((completed * 100) // len(gamepaths), completed, len(gamepaths), filename))
        pool.close()


    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate frame dump images for regression tests")
    parser.add_argument("-runner", action="store", required=True, help="Path to DuckStation regression test runner")
    parser.add_argument("-gamedir", action="append", required=True, help="Directory containing game images")
    parser.add_argument("-destdir", action="store", required=True, help="Base directory to dump frames to")
    parser.add_argument("-dumpinterval", action="store", type=int, default=600, help="Interval to dump frames at")
    parser.add_argument("-frames", action="store", type=int, default=36000, help="Number of frames to run")
    parser.add_argument("-parallel", action="store", type=int, default=1, help="Number of processes to run")
    parser.add_argument("-renderer", action="store", type=str, help="Renderer to use")
    parser.add_argument("-upscale", action="store", type=int, help="Upscale multiplier")
    parser.add_argument("-pgxp", action="store_true", help="Enable PGXP")
    parser.add_argument("-pgxpcpu", action="store_true", help="Enable PGXP CPU mode")
    parser.add_argument("-cpu", action="store", help="CPU execution mode")

    args = parser.parse_args()
    cargs = []
    if (args.upscale is not None):
        cargs += ["-upscale", str(args.upscale)]
    if (args.pgxp):
        cargs += ["-pgxp"]
    if (args.pgxpcpu):
        cargs += ["-pgxp-cpu"]
    if (args.cpu is not None):
        cargs += ["-cpu", args.cpu]

    if not run_regression_tests(args.runner, args.gamedir, os.path.realpath(args.destdir), args.dumpinterval, args.frames, args.parallel, args.renderer, cargs):
        sys.exit(1)
    else:
        sys.exit(0)

