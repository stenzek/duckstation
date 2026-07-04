#!/usr/bin/env python3
"""Export active unfinished Qt TS messages to editable JSONL batches."""

from __future__ import annotations

import argparse
import collections
import difflib
from pathlib import Path

if __package__ in (None, ""):
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from translation.ts_utils import (  # noqa: E402
    SCHEMA,
    SKIPPED_TYPES,
    CatalogMessage,
    catalog_fingerprint,
    message_to_batch_record,
    parse_catalog,
    write_jsonl,
)


def translated_text(message: CatalogMessage) -> str | list[str] | None:
    if message.plural_translations:
        return message.plural_translations if any(value.strip() for value in message.plural_translations) else None
    return message.translation if message.translation and message.translation.strip() else None


def build_suggestions(
    target: CatalogMessage,
    messages: list[CatalogMessage],
    limit: int,
    minimum_similarity: float,
) -> list[dict[str, object]]:
    if limit <= 0:
        return []
    candidates: list[tuple[float, CatalogMessage]] = []
    for candidate in messages:
        text = translated_text(candidate)
        if text is None or candidate.identifier == target.identifier:
            continue
        exact = candidate.identity.source == target.identity.source
        if not exact and candidate.identity.context != target.identity.context:
            continue
        similarity = (
            1.0
            if exact
            else difflib.SequenceMatcher(None, target.identity.source, candidate.identity.source).ratio()
        )
        if similarity >= minimum_similarity:
            candidates.append((similarity, candidate))
    candidates.sort(
        key=lambda pair: (
            pair[0],
            pair[1].translation_type == "finished",
            pair[1].identity.source == target.identity.source,
        ),
        reverse=True,
    )
    output: list[dict[str, object]] = []
    seen: set[tuple[str, str]] = set()
    for similarity, candidate in candidates:
        text = translated_text(candidate)
        signature = (candidate.identity.source, repr(text))
        if signature in seen:
            continue
        seen.add(signature)
        output.append(
            {
                "source": candidate.identity.source,
                "translation": text,
                "context": candidate.identity.context,
                "type": candidate.translation_type,
                "similarity": round(similarity, 4),
            }
        )
        if len(output) == limit:
            break
    return output


def split_balanced(records: list[dict[str, object]], batch_count: int) -> list[list[dict[str, object]]]:
    if batch_count <= 1 or len(records) <= 1:
        return [records]
    batch_count = min(batch_count, len(records))
    by_context: dict[str, list[dict[str, object]]] = collections.defaultdict(list)
    for record in records:
        by_context[str(record["context"])].append(record)

    target_size = max(1, (len(records) + batch_count - 1) // batch_count)
    chunks: list[list[dict[str, object]]] = []
    for context_records in by_context.values():
        if len(context_records) > target_size * 3 // 2:
            chunks.extend(
                context_records[offset : offset + target_size]
                for offset in range(0, len(context_records), target_size)
            )
        else:
            chunks.append(context_records)

    batches: list[list[dict[str, object]]] = [[] for _ in range(batch_count)]
    for chunk in sorted(chunks, key=len, reverse=True):
        destination = min(batches, key=len)
        destination.extend(chunk)
    return batches


def make_metadata(catalog: Path, fingerprint: str, batch_index: int, batch_count: int) -> dict[str, object]:
    return {
        "record_type": "metadata",
        "schema": SCHEMA,
        "catalog": str(catalog),
        "catalog_fingerprint": fingerprint,
        "batch_index": batch_index,
        "batch_count": batch_count,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", type=Path, help="Qt Linguist .ts catalog")
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument("--output", type=Path, help="single JSONL output file")
    output.add_argument("--batch-dir", type=Path, help="directory for numbered JSONL batches")
    parser.add_argument("--batches", type=int, default=1, help="number of balanced batches (with --batch-dir)")
    parser.add_argument("--context", action="append", default=[], help="include only this context; repeatable")
    parser.add_argument("--suggestions", type=int, default=2, help="translation-memory suggestions per message")
    parser.add_argument("--similarity", type=float, default=0.72, help="minimum fuzzy suggestion similarity")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.output and args.batches != 1:
        raise SystemExit("--batches requires --batch-dir")
    if args.batches < 1:
        raise SystemExit("--batches must be at least 1")
    if not 0.0 <= args.similarity <= 1.0:
        raise SystemExit("--similarity must be between 0 and 1")

    _, messages = parse_catalog(args.catalog)
    contexts = set(args.context)
    targets = [
        message
        for message in messages
        if message.translation_type == "unfinished"
        and message.translation_type not in SKIPPED_TYPES
        and (not contexts or message.identity.context in contexts)
    ]
    records = []
    for message in targets:
        record = message_to_batch_record(message)
        record["suggestions"] = build_suggestions(message, messages, args.suggestions, args.similarity)
        records.append(record)

    fingerprint = catalog_fingerprint(messages)
    if args.output:
        write_jsonl(args.output, make_metadata(args.catalog, fingerprint, 1, 1), records)
        destinations = [args.output]
    else:
        batches = split_balanced(records, args.batches)
        destinations = []
        for index, batch in enumerate(batches, 1):
            destination = args.batch_dir / f"batch-{index:03d}.jsonl"
            write_jsonl(destination, make_metadata(args.catalog, fingerprint, index, len(batches)), batch)
            destinations.append(destination)

    print(f"Exported {len(records)} active unfinished messages to {len(destinations)} file(s).")
    for destination in destinations:
        print(destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
