import json
import sys

root, host = sys.argv[1], sys.argv[2]
if root == host:
    raise SystemExit(0)


def rewrite(value):
    return value.replace(root + "/", host + "/")


with open("compile_commands.json", encoding="utf-8") as source:
    database = json.load(source)
for entry in database:
    for key in ("file", "directory", "output"):
        if key in entry:
            entry[key] = rewrite(entry[key])
    if "command" in entry:
        entry["command"] = rewrite(entry["command"])
    if "arguments" in entry:
        entry["arguments"] = [rewrite(arg) for arg in entry["arguments"]]
with open("compile_commands.json", "w", encoding="utf-8") as target:
    json.dump(database, target, indent=2)
