#!/usr/bin/env python3
"""Select TS catalogs for CI and reject structural regressions against a Git baseline."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

CATALOG_DIR = "src/duckstation-qt/translations"
VALIDATOR = Path(__file__).with_name("validate_ts.py")


def git(root: Path, *arguments: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *arguments], stderr=subprocess.PIPE)


def select_catalogs(root: Path, base: str | None, all_catalogs: bool, head: str = "HEAD") -> list[str]:
    if base is None:
        all_catalogs = True
        changed = []
    else:
        changed = git(root, "diff", "--name-only", "--no-renames", "-z", f"{base}..{head}").decode().split("\0")
        all_catalogs |= any(path.startswith(("scripts/translation/", ".agents/skills/duckstation-"))
                            or path == ".github/workflows/translation-lint.yml"
                            or (path.startswith(CATALOG_DIR + "/") and path.endswith((".sh", ".bat")))
                            for path in changed)
    if all_catalogs:
        return sorted(path for path in git(root, "ls-files", "-z", "--", f"{CATALOG_DIR}/*.ts").decode().split("\0")
                      if path and (root / path).is_file())
    return [path for path in changed if path.startswith(CATALOG_DIR + "/") and path.endswith(".ts")
            and (root / path).is_file()]


def lint(root: Path, base: str | None = None, all_catalogs: bool = False, head: str = "HEAD",
         selection_base: str | None = None) -> int:
    if base is not None:
        base = git(root, "rev-parse", "--verify", base + "^{commit}").decode().strip()
    paths = select_catalogs(root, selection_base or base, all_catalogs, head)
    if not paths:
        print("No translation files to validate.")
        return 0
    failed = False
    with tempfile.TemporaryDirectory(prefix="duckstation-translation-lint-") as directory:
        for index, relative in enumerate(paths):
            arguments = [sys.executable, "-B", str(VALIDATOR), str(root / relative), "--check-finished",
                         "--strict-required-tags", "--strict-placeholders", "--strict-accelerators",
                         "--reject-obsolete", "--reject-locations"]
            if base and git(root, "ls-tree", "--name-only", base, "--", relative).strip():
                baseline = Path(directory) / f"{index}.ts"
                baseline.write_bytes(git(root, "show", f"{base}:{relative}"))
                arguments.extend(["--baseline", str(baseline)])
            print(f"Validating {relative}", flush=True)
            result = subprocess.run(arguments, check=False)
            failed |= result.returncode != 0
    return int(failed)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="Git revision containing existing catalog defects")
    parser.add_argument("--all", action="store_true", help="validate every catalog, including unchanged files")
    parser.add_argument("--head", default="HEAD", help="select changes from this head rather than the checkout merge commit")
    parser.add_argument("--selection-base", help="select paths since this revision; --base remains the defect baseline")
    args = parser.parse_args()
    try:
        root = Path(git(Path.cwd(), "rev-parse", "--show-toplevel").decode().strip())
        return lint(root, args.base, args.all, args.head, args.selection_base)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"ERROR: unable to select or validate catalogs: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
