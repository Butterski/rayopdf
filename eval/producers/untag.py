#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Make a tagged PDF untagged, in place: the catalog's /StructTreeRoot and /MarkInfo and every
page's /StructParents are deleted (the chromium-untagged producer of eval/producers/render.py).
The pages' marked content stays; without a structure tree it ties nothing to anything.

Usage: untag.py FILE.pdf
"""

import argparse

import pikepdf


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pdf")
    args = parser.parse_args()
    with pikepdf.open(args.pdf, allow_overwriting_input=True) as pdf:
        for key in ("/StructTreeRoot", "/MarkInfo"):
            if key in pdf.Root:
                del pdf.Root[key]
        for page in pdf.pages:
            if "/StructParents" in page.obj:
                del page.obj["/StructParents"]
        pdf.save(args.pdf)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
