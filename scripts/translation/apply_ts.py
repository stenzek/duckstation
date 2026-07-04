#!/usr/bin/env python3
"""Safely apply completed JSONL translation batches to a Qt TS catalog."""

from __future__ import annotations

import argparse
import os
import tempfile
from pathlib import Path

if __package__ in (None, ""):
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from translation.ts_utils import (  # noqa: E402
    SCHEMA,
    MessageIdentity,
    catalog_fingerprint,
    load_jsonl,
    parse_catalog,
    placeholders_match,
    replace_translation_node,
    scan_raw_messages,
    validate_translation,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", type=Path, help="Qt Linguist .ts catalog")
    parser.add_argument("batches", nargs="+", type=Path, help="completed JSONL batch files")
    parser.add_argument("--write", action="store_true", help="atomically update the catalog; default is dry-run")
    return parser.parse_args()


def record_identity(record: dict[str, object]) -> MessageIdentity:
    return MessageIdentity(
        context=str(record.get("context", "")),
        source=str(record.get("source", "")),
        comment=str(record.get("comment", "")),
        extracomment=str(record.get("extracomment", "")),
        numerus=bool(record.get("numerus", False)),
        occurrence=int(record.get("occurrence", 0)),
    )


def resolve_target(record: dict[str, object]) -> tuple[str | None, list[str] | None] | None:
    singular = record.get("target_translation")
    plurals = record.get("target_plural_translations")
    accept_current = record.get("accept_current") is True
    if accept_current:
        if singular is not None or plurals is not None:
            raise ValueError(f"{record.get('id')}: accept_current cannot be combined with target fields")
        current_plurals = record.get("current_plural_translations")
        if current_plurals:
            plurals = current_plurals
        else:
            singular = record.get("current_translation")

    if singular is None and plurals is None:
        return None
    if singular is not None and plurals is not None:
        raise ValueError(f"{record.get('id')}: set either singular or plural target, not both")
    if singular is not None:
        if not isinstance(singular, str) or not singular.strip():
            raise ValueError(f"{record.get('id')}: target_translation must be a nonempty string")
        return singular, None
    if (
        not isinstance(plurals, list)
        or not plurals
        or any(not isinstance(value, str) or not value.strip() for value in plurals)
    ):
        raise ValueError(f"{record.get('id')}: target_plural_translations must contain nonempty strings")
    return None, plurals


def atomic_write(path: Path, text: str) -> None:
    mode = path.stat().st_mode
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="", dir=path.parent, delete=False) as stream:
            temporary_name = stream.name
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary_name, mode)
        os.replace(temporary_name, path)
    finally:
        if temporary_name and os.path.exists(temporary_name):
            os.unlink(temporary_name)


def main() -> int:
    args = parse_args()
    metadata, records = load_jsonl(args.batches)
    if len(metadata) != len(args.batches):
        raise SystemExit("each batch must contain exactly one metadata record")
    for item in metadata:
        if item.get("schema") != SCHEMA:
            raise SystemExit(f"unsupported batch schema: {item.get('schema')!r}")

    _, messages = parse_catalog(args.catalog)
    fingerprint = catalog_fingerprint(messages)
    expected_fingerprints = {str(item.get("catalog_fingerprint", "")) for item in metadata}
    if expected_fingerprints != {fingerprint}:
        raise SystemExit("catalog source identity has changed since extraction; export fresh batches")

    with args.catalog.open("r", encoding="utf-8", newline="") as stream:
        raw_text = stream.read()
    spans = scan_raw_messages(raw_text)
    seen: set[str] = set()
    replacements: list[tuple[int, int, str]] = []
    skipped = 0
    for record in records:
        identifier = str(record.get("id", ""))
        identity = record_identity(record)
        if identifier != identity.identifier:
            raise SystemExit(f"{identifier or '<missing id>'}: record identity does not match its id")
        if identifier in seen:
            raise SystemExit(f"duplicate record id across batches: {identifier}")
        seen.add(identifier)
        target = resolve_target(record)
        if target is None:
            skipped += 1
            continue
        span = spans.get(identifier)
        if span is None:
            raise SystemExit(f"message no longer exists in catalog: {identifier}")
        if span.translation_type != "unfinished":
            raise SystemExit(f"refusing to overwrite {span.translation_type} translation: {identifier}")
        singular, plurals = target
        if span.numerus != (plurals is not None):
            raise SystemExit(f"singular/plural target does not match catalog message: {identifier}")
        values = plurals if plurals is not None else [singular or ""]
        for value in values:
            problems = validate_translation(
                identity.source,
                value,
                allow_missing_placeholders=plurals is not None,
            )
            if problems:
                raise SystemExit(f"{identifier}: {'; '.join(problems)}")
        if plurals is not None and not any(placeholders_match(identity.source, value) for value in plurals):
            raise SystemExit(f"{identifier}: no plural form preserves the complete source placeholder set")
        replacement_block = replace_translation_node(span.block, singular, plurals)
        replacements.append((span.start, span.end, replacement_block))

    updated_text = raw_text
    for start, end, replacement in sorted(replacements, reverse=True):
        updated_text = updated_text[:start] + replacement + updated_text[end:]
    if args.write and replacements:
        atomic_write(args.catalog, updated_text)

    action = "Applied" if args.write else "Would apply"
    print(f"{action} {len(replacements)} translation(s); skipped {skipped} record(s) without targets.")
    if not args.write:
        print("Dry-run only; pass --write to update the catalog.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
