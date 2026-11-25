#!/usr/bin/env python3
"""Checks the licenses of the vcpkg packages built for one triplet.

- No GPL: a package fails when every way of taking its license includes the
  GPL. LGPL is fine, and for "A OR B" one acceptable choice is enough.
- LGPL packages must be shared libraries (no static archives), so users can
  replace them; see docs/licenses.md in the main GeoCrew repository.
- A package without a license in its vcpkg metadata fails until it is
  reviewed and added to KNOWN below.

Usage: scripts/check-licenses.py <vcpkg_installed dir> <triplet>
"""
import json
import pathlib
import re
import sys

# Ports whose vcpkg metadata records no license, reviewed by hand. Only what we
# ship counts: libiconv's iconv program is GPL, but only its LGPL library ships.
KNOWN = {
    "gdal": "MIT",
    "libiconv": "LGPL-2.1-or-later",
}

GPL = re.compile(r"(?<!L)GPL")  # GPL and AGPL, not LGPL


def choices(expression):
    """The ways of taking an SPDX license expression, each a set of licenses that all apply.

    "LGPL-2.1-only AND (Apache-2.0 OR BSL-1.0)" gives
    [{LGPL-2.1-only, Apache-2.0}, {LGPL-2.1-only, BSL-1.0}].
    """
    tokens = re.findall(r"[()]|[^\s()]+", expression)
    pos = 0

    def take():
        nonlocal pos
        pos += 1
        return tokens[pos - 1]

    def peek():
        return tokens[pos] if pos < len(tokens) else None

    def primary():
        if peek() == "(":
            take()
            result = disjunction()
            take()  # ")"
            return result
        license = take()
        if peek() == "WITH":  # an exception does not change which license applies
            take()
            take()
        return [frozenset([license])]

    def conjunction():
        result = primary()
        while peek() == "AND":
            take()
            right = primary()
            result = [a | b for a in result for b in right]
        return result

    def disjunction():
        result = conjunction()
        while peek() == "OR":
            take()
            result += conjunction()
        return result

    return disjunction()


def main(installed, triplet):
    root = pathlib.Path(installed)
    failures = []
    for spdx in sorted(root.glob(f"{triplet}/share/*/vcpkg.spdx.json")):
        port = spdx.parent.name
        doc = json.loads(spdx.read_text(encoding="utf-8"))
        package = next(p for p in doc["packages"] if p.get("SPDXID") == "SPDXRef-port")
        license = package.get("licenseConcluded", "NOASSERTION")
        if license == "NOASSERTION":
            license = KNOWN.get(port, license)
        note = ""
        acceptable = [c for c in choices(license) if not any(GPL.search(l) for l in c)]
        if license == "NOASSERTION":
            failures.append(f"{port}: no license recorded; review it and add it to KNOWN")
            note = "  <- unknown"
        elif not acceptable:
            failures.append(f"{port}: GPL ({license})")
            note = "  <- GPL"
        elif all(any("LGPL" in l for l in c) for c in acceptable):
            files = []
            for info in root.glob(f"vcpkg/info/{port}_*_{triplet}.list"):
                files += info.read_text(encoding="utf-8").split()
            static = [f for f in files if f.endswith(".a")]
            if static:
                failures.append(f"{port}: LGPL but built as a static library ({', '.join(static)})")
                note = "  <- LGPL built static"
            else:
                note = "  (LGPL, shared library)"
        print(f"{port:24} {license}{note}")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
