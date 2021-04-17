import json
import sys
import os

if __name__ == "__main__":
    if (len(sys.argv) < 3):
        print("usage: %s <gamedb dir> <output path>" % sys.argv[0])
        sys.exit(1)

    games = []
    for file in os.listdir(sys.argv[1]):
        with open(os.path.join(sys.argv[1], file), "r") as f:
            fgames = json.load(f)
            games.extend(list(fgames))


    with open(sys.argv[2], "w") as f:
        json.dump(games, f, indent=1)

    print("Wrote %s" % sys.argv[2])