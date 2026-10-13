#!/usr/bin/env python3
"""Extract every file from an Xbox 360 STFS package (CON/LIVE/PIRS)."""

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _stfs_reader import StfsReader  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--list", action="store_true", help="list entries without extracting")
    args = parser.parse_args()

    reader = StfsReader(args.package)
    entries = reader.list_entries()
    for entry in entries:
        target = args.out_dir / entry["path"]
        if entry["is_dir"]:
            print(f"  <dir>      {entry['path']}/")
            if not args.list:
                target.mkdir(parents=True, exist_ok=True)
            continue
        print(f"  {entry['length']:>10}  {entry['path']}")
        if not args.list:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(reader.read_file(entry["start_block"], entry["length"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
