#!/usr/bin/env python3
"""Validate Qt TS structure, completeness, placeholders, plurals, and rich text."""

from __future__ import annotations

import argparse
import collections
import xml.etree.ElementTree as ET
from pathlib import Path

if __package__ in (None, ""):
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from translation.ts_utils import (  # noqa: E402
    SKIPPED_TYPES,
    CatalogMessage,
    extra_rich_tags,
    extract_placeholders,
    load_jsonl,
    missing_rich_tags,
    parse_catalog,
    placeholder_counts_match,
    placeholders_match,
    unbalanced_rich_tags,
    validate_translation,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", type=Path, help="Qt Linguist .ts catalog")
    parser.add_argument("--require-complete", action="store_true", help="fail on unfinished or empty active messages")
    parser.add_argument("--records", nargs="+", type=Path, help="validate only IDs contained in JSONL batches")
    parser.add_argument(
        "--placeholders-only",
        action="store_true",
        help="check placeholders only, allowing incomplete translations",
    )
    parser.add_argument("--strict-extra-tags", action="store_true", help="treat added rich-text tags as errors")
    return parser.parse_args()


def values_for(message: CatalogMessage) -> list[str]:
    return message.plural_translations if message.plural_translations else [message.translation or ""]


def diagnostic_label(catalog: Path, message: CatalogMessage) -> str:
    line = message.catalog_line or 1
    label = f"{catalog}:{line}: {message.identity.context}/{message.identifier}"
    source_locations = []
    for location in message.locations:
        filename = location.get("filename", "")
        source_line = location.get("line", "")
        if filename and source_line:
            source_locations.append(f"{filename}:{source_line}")
        elif filename:
            source_locations.append(filename)
    if source_locations:
        label += f" [source: {', '.join(source_locations)}]"
    return label


def main() -> int:
    args = parse_args()
    try:
        _, messages = parse_catalog(args.catalog)
    except ET.ParseError as error:
        line, column = error.position
        print(f"ERROR: {args.catalog}:{line}:{column + 1}: XML parse error: {error}")
        return 1
    except (OSError, ValueError) as error:
        print(f"ERROR: {args.catalog}:1: unable to read catalog: {error}")
        return 1

    selected_ids: set[str] | None = None
    if args.records:
        _, records = load_jsonl(args.records)
        selected_ids = {str(record.get("id", "")) for record in records}

    errors: list[str] = []
    warnings: list[str] = []
    checked = 0
    type_counts: collections.Counter[str] = collections.Counter()
    for message in messages:
        type_counts[message.translation_type] += 1
        if message.translation_type in SKIPPED_TYPES:
            continue
        if selected_ids is not None and message.identifier not in selected_ids:
            continue
        checked += 1
        label = diagnostic_label(args.catalog, message)
        if not args.placeholders_only:
            if message.identity.numerus and not message.plural_translations:
                errors.append(f"{label}: numerus message has no <numerusform> forms")
            if not message.identity.numerus and message.plural_translations:
                errors.append(f"{label}: singular message unexpectedly has plural forms")
            if args.require_complete and message.translation_type == "unfinished":
                errors.append(f"{label}: active translation is unfinished")
            if args.require_complete and not any(value.strip() for value in values_for(message)):
                errors.append(f"{label}: active translation is empty")

        for value in values_for(message):
            if not value.strip() and not args.require_complete:
                continue
            problems = validate_translation(
                message.identity.source,
                value,
                allow_missing_placeholders=message.identity.numerus,
            )
            for problem in problems:
                if args.placeholders_only and not problem.startswith("placeholder mismatch"):
                    continue
                errors.append(f"{label}: {problem}")
            if placeholders_match(message.identity.source, value) and not placeholder_counts_match(
                message.identity.source, value
            ):
                source_placeholders = extract_placeholders(message.identity.source)
                translated_placeholders = extract_placeholders(value)
                warnings.append(
                    f"{label}: placeholder count mismatch: source={dict(source_placeholders)} "
                    f"translation={dict(translated_placeholders)}"
                )
            if not args.placeholders_only:
                extras = extra_rich_tags(message.identity.source, value)
                if extras:
                    output = errors if args.strict_extra_tags else warnings
                    output.append(f"{label}: additional rich-text tags: {dict(extras)}")
                extras = missing_rich_tags(message.identity.source, value)
                if extras:
                    output = errors if args.strict_extra_tags else warnings
                    output.append(f"{label}: missing rich-text tags: {dict(extras)}")
                unbalanced = unbalanced_rich_tags(value)
                if unbalanced:
                    output = errors if args.strict_extra_tags else warnings
                    output.append(f"{label}: unbalanced rich-text tags (opening, closing): {unbalanced}")
        if (
            message.identity.numerus
            and any(value.strip() for value in values_for(message))
            and not any(placeholders_match(message.identity.source, value) for value in values_for(message))
        ):
            errors.append(f"{label}: no plural form preserves the complete source placeholder set")

    if selected_ids is not None:
        found = {message.identifier for message in messages}
        for missing in sorted(selected_ids - found):
            errors.append(f"{args.catalog}:1: {missing}: batch record not found in catalog")

    print(f"Checked {checked} active message(s). Translation types: {dict(type_counts)}")
    for warning in warnings:
        print(f"WARNING: {warning}")
    for error in errors:
        print(f"ERROR: {error}")
    if errors:
        print(f"Validation failed with {len(errors)} error(s) and {len(warnings)} warning(s).")
        return 1
    print(f"Validation passed with {len(warnings)} warning(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
