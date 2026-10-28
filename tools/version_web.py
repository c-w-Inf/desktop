import os
import sys

root = sys.argv[1]
version = "v20261026f"
renames = {}
for name in ("image_canvas.js", "image_canvas.wasm", "image_canvas.data"):
    old = os.path.join(root, name)
    if not os.path.exists(old):
        continue
    new_name = name.replace("image_canvas.", "image_canvas." + version + ".")
    os.replace(old, os.path.join(root, new_name))
    renames[name] = new_name

for page in os.listdir(root):
    if not page.endswith((".html", ".js")):
        continue
    path = os.path.join(root, page)
    with open(path, "rb") as source:
        text = source.read()
    for old_name, new_name in renames.items():
        text = text.replace(old_name.encode(), new_name.encode())
    with open(path, "wb") as target:
        target.write(text)
