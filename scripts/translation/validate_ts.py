#!/usr/bin/env python3
"""Validate Qt TS structure, metadata, completeness, placeholders, plurals, and rich text."""

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
    accelerator_count,
    extra_rich_tags,
    extract_placeholders,
    load_jsonl,
    missing_rich_tags,
    parse_catalog,
    placeholder_counts_match,
    placeholders_match,
    rich_text_problems,
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
    parser.add_argument(
        "--strict-required-tags",
        action="store_true",
        help="treat missing or unbalanced rich-text tags as errors",
    )
    parser.add_argument("--strict-extra-tags", action="store_true", help="treat added rich-text tags as errors")
    parser.add_argument("--strict-placeholders", action="store_true", help="reject changed placeholder counts")
    parser.add_argument("--strict-accelerators", action="store_true", help="reject lost or added menu accelerators")
    parser.add_argument("--check-finished", action="store_true", help="reject empty forms marked finished")
    parser.add_argument("--baseline", type=Path, help="report existing baseline defects separately from new errors")
    parser.add_argument(
        "--reject-obsolete",
        action="store_true",
        help="fail if the catalog contains obsolete or vanished messages",
    )
    parser.add_argument(
        "--reject-locations",
        action="store_true",
        help="fail if the catalog contains source location information",
    )
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


def message_diagnostics(message: CatalogMessage, args: argparse.Namespace) -> list[tuple[str, str]]:
    diagnostics: list[tuple[str, str]] = []
    if message.translation_type in SKIPPED_TYPES:
        return diagnostics
    source = message.identity.source
    values = values_for(message)
    if not args.placeholders_only:
        if message.identity.numerus and not message.plural_translations:
            diagnostics.append(("ERROR", "numerus message has no <numerusform> forms"))
        if not message.identity.numerus and message.plural_translations:
            diagnostics.append(("ERROR", "singular message unexpectedly has plural forms"))
        if args.require_complete and message.translation_type in {"unfinished", "missing"}:
            diagnostics.append(("ERROR", "active translation is unfinished or missing"))
        if args.require_complete or (args.check_finished and message.translation_type == "finished"):
            for index, value in enumerate(values, 1):
                if not value.strip():
                    detail = f"plural form {index} is empty" if message.identity.numerus else "active translation is empty"
                    diagnostics.append(("ERROR", detail))
    for index, value in enumerate(values, 1):
        if not value.strip():
            continue
        prefix = f"plural form {index}: " if message.identity.numerus else ""
        for problem in validate_translation(source, value, allow_missing_placeholders=message.identity.numerus):
            if not args.placeholders_only or problem.startswith(("placeholder mismatch", "fmt argument")):
                diagnostics.append(("ERROR", prefix + problem))
        if (not message.identity.numerus and placeholders_match(source, value)
                and not placeholder_counts_match(source, value)):
            detail = (
                f"placeholder count mismatch: source={dict(extract_placeholders(source))} "
                f"translation={dict(extract_placeholders(value))}"
            )
            diagnostics.append(("ERROR" if args.strict_placeholders else "WARNING", prefix + detail))
        if not args.placeholders_only:
            extras = extra_rich_tags(source, value)
            if extras:
                diagnostics.append(("ERROR" if args.strict_extra_tags else "WARNING",
                                    prefix + f"additional rich-text tags: {dict(extras)}"))
            required_level = "ERROR" if args.strict_required_tags or args.strict_extra_tags else "WARNING"
            missing = missing_rich_tags(source, value)
            if missing:
                diagnostics.append((required_level, prefix + f"missing rich-text tags: {dict(missing)}"))
            unbalanced = unbalanced_rich_tags(value)
            if unbalanced:
                diagnostics.append((required_level, prefix + f"unbalanced rich-text tags: {unbalanced}"))
            diagnostics.extend((required_level, prefix + problem) for problem in rich_text_problems(source, value))
            if args.strict_accelerators and accelerator_count(source) != accelerator_count(value):
                diagnostics.append(("ERROR", prefix + "accelerator count mismatch"))
    if (message.identity.numerus and any(value.strip() for value in values)
            and not any(placeholder_counts_match(source, value) for value in values)):
        diagnostics.append(("ERROR", "no plural form preserves the complete source placeholder set"))
    return diagnostics


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

    baseline: dict[str, set[tuple[str, str]]] = {}
    baseline_values: dict[str, tuple] = {}
    if args.baseline:
        try:
            _, old_messages = parse_catalog(args.baseline)
        except (OSError, ValueError, ET.ParseError) as error:
            print(f"ERROR: unable to read baseline: {error}")
            return 1
        baseline = {message.identifier: set(message_diagnostics(message, args)) for message in old_messages}
        baseline_values = {message.identifier: (message.translation_type, values_for(message)) for message in old_messages}

    errors: list[str] = []
    warnings: list[str] = []
    checked = 0
    type_counts: collections.Counter[str] = collections.Counter()
    known_errors = 0
    for message in messages:
        type_counts[message.translation_type] += 1
        label = diagnostic_label(args.catalog, message)
        if args.reject_obsolete and message.translation_type in SKIPPED_TYPES:
            errors.append(f"{label}: {message.translation_type} message is not permitted")
        if args.reject_locations and message.locations:
            errors.append(f"{label}: source location information is not permitted")
        if message.translation_type in SKIPPED_TYPES:
            continue
        if selected_ids is not None and message.identifier not in selected_ids:
            continue
        checked += 1
        for level, detail in message_diagnostics(message, args):
            if (level == "ERROR" and (level, detail) in baseline.get(message.identifier, set())
                    and baseline_values[message.identifier] == (message.translation_type, values_for(message))):
                known_errors += 1
                continue
            (errors if level == "ERROR" else warnings).append(f"{label}: {detail}")

    if selected_ids is not None:
        found = {message.identifier for message in messages}
        for missing in sorted(selected_ids - found):
            errors.append(f"{args.catalog}:1: {missing}: batch record not found in catalog")

    print(f"Checked {checked} active message(s). Translation types: {dict(type_counts)}")
    if args.baseline:
        print(f"Baseline defects: {known_errors} existing error(s); new defects fail validation.")
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
