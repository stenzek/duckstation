from importlib.machinery import FileFinder
import json
import sys
import urllib.request

if len(sys.argv) < 3:
    print(f"Syntax: {sys.argv[0]} WEB-API-KEY OUTPUT_FILE.yaml")
    sys.exit(1)

print("Sending request...")
url = "https://retroachievements.org/API/API_GetGameList.php?y=" + sys.argv[1] + "&i=12&h=1&f=0"
req = urllib.request.Request(url)
req.add_header("User-Agent", "curl/8.10.1") # CF blocks urllib user agent
data = None
with urllib.request.urlopen(req) as response:
    data = response.read().decode('utf-8')

data = json.loads(data)
print(f"Found {len(data)} entries in JSON")

games = {}
hashes = {}
for entry in data:
    if ("ID" not in entry or \
       "NumAchievements" not in entry or \
       "Hashes" not in entry or \
       len(entry["Hashes"]) == 0):
        continue

    game_id = int(entry["ID"])
    num_achievements = int(entry["NumAchievements"])

    if game_id not in games:
        games[game_id] = num_achievements
    else:
        print(f"Duplicate game {game_id}")

    for thash in entry["Hashes"]:
        if thash in hashes:
            print(f"Duplicate hash {thash}")
            continue
        hashes[thash] = game_id

print(f"Extracted {len(games)} games")
print(f"Extracted {len(hashes)} hashes")

with open(sys.argv[2], "w") as f:
    f.write("hashes:\n")
    for ghash, game_id in hashes.items():
        f.write(f"  {ghash}: {game_id}\n")

    f.write("achievements:\n")
    for game_id, num_achievements in games.items():
        f.write(f"  {game_id}: {num_achievements}\n")
