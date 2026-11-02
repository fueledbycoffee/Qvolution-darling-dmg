#!/usr/bin/env python3
# Extracts a disk image with darling-dmg-extract and compares one bundle inside it
# against a JSON manifest of the expected files: a list of
# {"path", "type": "file", "size", "sha256"} or {"path", "type": "symlink", "target"},
# with paths relative to the bundle.
#
# Usage: check-extract.py <darling-dmg-extract> <image> <manifest.json> <bundle path in image>

import hashlib
import json
import os
import subprocess
import sys
import tempfile


def scan(root):
    found = {}
    for directory, dirs, files in os.walk(root):
        for name in dirs + files:
            full = os.path.join(directory, name)
            rel = os.path.relpath(full, root)
            if os.path.islink(full):
                found[rel] = {"type": "symlink", "target": os.readlink(full)}
                if name in dirs:
                    dirs.remove(name)
            elif os.path.isfile(full):
                digest = hashlib.sha256()
                with open(full, "rb") as f:
                    for block in iter(lambda: f.read(1 << 20), b""):
                        digest.update(block)
                found[rel] = {"type": "file", "size": os.path.getsize(full),
                              "sha256": digest.hexdigest()}
    return found


def main():
    if len(sys.argv) != 5:
        sys.exit("usage: check-extract.py <extractor> <image> <manifest> <bundle>")
    extractor, image, manifest, bundle = sys.argv[1:]
    with open(manifest) as f:
        expected = {entry["path"]: {k: v for k, v in entry.items() if k != "path"}
                    for entry in json.load(f)}

    # Extract beside the build rather than into /tmp, which is often a small tmpfs.
    with tempfile.TemporaryDirectory(dir=os.getcwd()) as destination:
        subprocess.run([extractor, image, destination], check=True)
        found = scan(os.path.join(destination, bundle))

    problems = []
    for path in sorted(expected.keys() | found.keys()):
        want, got = expected.get(path), found.get(path)
        if want is None:
            problems.append("unexpected: " + path)
        elif got is None:
            problems.append("missing: " + path)
        elif want != got:
            problems.append("differs: %s: expected %s, got %s" % (path, want, got))
    for problem in problems:
        print(problem)
    print("%d entries expected, %d found, %d problems" % (len(expected), len(found), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
