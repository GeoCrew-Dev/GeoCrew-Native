#!/usr/bin/env python3
"""Checks the licenses of the vcpkg packages built for one triplet.

- No GPL: a package fails when every way of taking its license includes the
  GPL. LGPL is fine, and for "A OR B" one acceptable choice is enough.
- LGPL packages must be shared libraries (no static archives), so users can
  replace them; see docs/licenses.md in the main GeoCrew repository. The
  server's helper may link them statically (--allow-static-lgpl): the project
  is fully open source, so users can relink it.
- A package without a license in its vcpkg metadata fails until it is
  reviewed and added to KNOWN below.

Usage: scripts/check-licenses.py [--allow-static-lgpl] <vcpkg_installed dir> <triplet>
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
    # Build tools only, never shipped. They land in the same tree when the target
    # triplet is also the host triplet, as for the server helper on Linux and macOS.
    "pkgconf": "ISC",
    "vcpkg-make": "MIT",
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


def main(installed, triplet, allow_static_lgpl=False):
    root = pathlib.Path(installed)
    failures = []
    found = sorted(root.glob(f"{triplet}/share/*/vcpkg.spdx.json"))
    if not found:
        print(f"no packages found under {root / triplet}", file=sys.stderr)
        return 1
    for spdx in found:
        port = spdx.parent.name
        doc = json.loads(spdx.read_text(encoding="utf-8"))
        package = next(p for p in doc["packages"] if p.get("SPDXID") == "SPDXRef-port")
        license = package.get("licenseConcluded", "NOASSERTION")
        # vcpkg writes LicenseRef-vcpkg-null for a port whose manifest has no license.
        if license in ("NOASSERTION", "LicenseRef-vcpkg-null"):
            license = KNOWN.get(port, "NOASSERTION")
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
            # Windows static libraries are .lib too, but so are import libraries of DLLs.
            has_dll = any(f.endswith(".dll") for f in files)
            static = [f for f in files if f.endswith(".a") or (f.endswith(".lib") and not has_dll)]
            if static and not allow_static_lgpl:
                failures.append(f"{port}: LGPL but built as a static library ({', '.join(static)})")
                note = "  <- LGPL built static"
            else:
                note = "  (LGPL, static: allowed for the server helper)" if static else "  (LGPL, shared library)"
        print(f"{port:24} {license}{note}")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    allow = "--allow-static-lgpl" in args
    args = [a for a in args if a != "--allow-static-lgpl"]
    if len(args) != 2:
        sys.exit(__doc__)
    sys.exit(main(args[0], args[1], allow))
