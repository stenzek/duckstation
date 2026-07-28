#!/usr/bin/env python3
"""Create, resume, validate, and atomically apply Qt translation sessions."""

from __future__ import annotations

import argparse
import collections
import datetime
import difflib
import json
import os
import shutil
import sys
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Sequence

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from translation.ts_utils import (  # noqa: E402
    SKIPPED_TYPES,
    CatalogMessage,
    atomic_write_text,
    catalog_fingerprint,
    extra_rich_tags,
    iter_catalog_messages,
    jsonl_text,
    message_to_task_record,
    missing_rich_tags,
    parse_catalog,
    placeholder_counts_match,
    replace_translation_node,
    scan_raw_messages,
    text_sha256,
    unbalanced_rich_tags,
    validate_translation,
)


SESSION_SCHEMA = "duckstation-qt-translation-session-v2"
TASK_SCHEMA = "duckstation-qt-translation-task-v2"
RESPONSE_SCHEMA = "duckstation-qt-translation-response-v2"
REVIEW_SCHEMA = "duckstation-qt-translation-reviews-v2"
DECISION_FIELDS = ("translation", "plural_translations", "accept_current", "accept_source")
DEFAULT_MAX_RECORDS = 300
DEFAULT_MAX_BYTES = 192 * 1024
TASK_METADATA_RESERVE = 256


def json_line(value: dict[str, object]) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True)


def read_jsonl(path: Path) -> list[dict[str, object]]:
    output: list[dict[str, object]] = []
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {error}") from error
            if not isinstance(item, dict):
                raise ValueError(f"{path}:{line_number}: JSONL records must be objects")
            output.append(item)
    return output


def read_json(path: Path) -> dict[str, object]:
    try:
        item = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"{path}: invalid JSON: {error}") from error
    if not isinstance(item, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return item


def translated_text(message: CatalogMessage) -> str | list[str] | None:
    if message.plural_translations:
        return message.plural_translations if any(value.strip() for value in message.plural_translations) else None
    return message.translation if message.translation and message.translation.strip() else None


def build_suggestion_indexes(
    messages: Sequence[CatalogMessage],
) -> tuple[dict[str, list[CatalogMessage]], dict[str, list[CatalogMessage]]]:
    exact: dict[str, list[CatalogMessage]] = collections.defaultdict(list)
    contexts: dict[str, list[CatalogMessage]] = collections.defaultdict(list)
    for message in messages:
        if translated_text(message) is None:
            continue
        exact[message.identity.source].append(message)
        contexts[message.identity.context].append(message)
    return exact, contexts


def build_suggestions(
    target: CatalogMessage,
    exact_index: dict[str, list[CatalogMessage]],
    context_index: dict[str, list[CatalogMessage]],
    limit: int,
    minimum_similarity: float,
) -> list[dict[str, object]]:
    if limit <= 0:
        return []
    candidates: list[tuple[float, CatalogMessage]] = []
    visited: set[str] = set()
    for candidate in exact_index.get(target.identity.source, []):
        if candidate.identifier != target.identifier:
            candidates.append((1.0, candidate))
            visited.add(candidate.identifier)
    for candidate in context_index.get(target.identity.context, []):
        if candidate.identifier == target.identifier or candidate.identifier in visited:
            continue
        similarity = difflib.SequenceMatcher(None, target.identity.source, candidate.identity.source).ratio()
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
    suggestions: list[dict[str, object]] = []
    seen: set[tuple[str, str]] = set()
    for similarity, candidate in candidates:
        text = translated_text(candidate)
        signature = (candidate.identity.source, repr(text))
        if signature in seen:
            continue
        seen.add(signature)
        suggestions.append(
            {
                "source": candidate.identity.source,
                "translation": text,
                "context": candidate.identity.context,
                "type": candidate.translation_type,
                "similarity": round(similarity, 4),
            }
        )
        if len(suggestions) == limit:
            break
    return suggestions


def task_record(message: CatalogMessage, suggestions: list[dict[str, object]]) -> dict[str, object]:
    record = message_to_task_record(message)
    record["suggestions"] = suggestions
    return record


def split_tasks(
    records: Sequence[dict[str, object]], max_records: int, max_bytes: int
) -> list[list[dict[str, object]]]:
    tasks: list[list[dict[str, object]]] = []
    current: list[dict[str, object]] = []
    current_bytes = TASK_METADATA_RESERVE
    for record in records:
        record_bytes = len((json_line(record) + "\n").encode("utf-8"))
        if TASK_METADATA_RESERVE + record_bytes > max_bytes:
            raise ValueError(
                f"{record.get('id')}: serialized record exceeds --max-bytes; "
                "increase the limit for this catalog"
            )
        if current and (len(current) >= max_records or current_bytes + record_bytes > max_bytes):
            tasks.append(current)
            current = []
            current_bytes = TASK_METADATA_RESERVE
        current.append(record)
        current_bytes += record_bytes
    if current:
        tasks.append(current)
    return tasks


def find_repo_root(path: Path) -> Path:
    for candidate in (path, *path.parents):
        if (candidate / ".git").exists():
            return candidate
    return Path.cwd().resolve()


def session_file(session: Path, relative: object) -> Path:
    path = (session / str(relative)).resolve()
    try:
        path.relative_to(session.resolve())
    except ValueError as error:
        raise ValueError(f"session artifact escapes session directory: {relative}") from error
    return path


def state_for_message(message: CatalogMessage) -> dict[str, object]:
    return {
        "type": message.translation_type,
        "translation": message.translation,
        "plural_translations": message.plural_translations,
    }


def state_for_record(record: dict[str, object]) -> dict[str, object]:
    return {
        "type": record.get("current_type"),
        "translation": record.get("current_translation"),
        "plural_translations": record.get("current_plural_translations") or [],
    }


def load_session(
    session_value: Path,
) -> tuple[Path, dict[str, object], list[dict[str, object]], dict[str, dict[str, object]]]:
    session = session_value.resolve()
    manifest = read_json(session / "manifest.json")
    if manifest.get("schema") != SESSION_SCHEMA:
        raise ValueError(f"{session}: unsupported session schema: {manifest.get('schema')!r}")
    task_records: list[dict[str, object]] = []
    tasks_by_id: dict[str, dict[str, object]] = {}
    seen_ids: set[str] = set()
    tasks = manifest.get("tasks")
    if not isinstance(tasks, list):
        raise ValueError(f"{session}: manifest tasks must be a list")
    for task_value in tasks:
        if not isinstance(task_value, dict):
            raise ValueError(f"{session}: invalid task manifest record")
        task_id = str(task_value.get("id", ""))
        if not task_id or task_id in tasks_by_id:
            raise ValueError(f"{session}: missing or duplicate task id: {task_id!r}")
        task_path = session_file(session, task_value.get("path"))
        task_text = task_path.read_text(encoding="utf-8")
        if text_sha256(task_text) != task_value.get("sha256"):
            raise ValueError(f"{task_path}: immutable task checksum mismatch")
        lines = read_jsonl(task_path)
        if not lines or lines[0] != {
            "record_type": "metadata",
            "schema": TASK_SCHEMA,
            "session_id": manifest.get("session_id"),
            "task_id": task_id,
        }:
            raise ValueError(f"{task_path}: invalid task metadata")
        records = lines[1:]
        expected_ids = task_value.get("record_ids")
        actual_ids = [record.get("id") for record in records]
        if actual_ids != expected_ids:
            raise ValueError(f"{task_path}: task record list does not match manifest")
        for record in records:
            identifier = str(record.get("id", ""))
            if record.get("record_type") != "message" or not identifier or identifier in seen_ids:
                raise ValueError(f"{task_path}: invalid or duplicate message id: {identifier!r}")
            seen_ids.add(identifier)
        task_value = dict(task_value)
        task_value["records"] = records
        tasks_by_id[task_id] = task_value
        task_records.extend(records)
    if len(task_records) != manifest.get("target_count"):
        raise ValueError(f"{session}: manifest target count does not match tasks")
    return session, manifest, task_records, tasks_by_id


def load_reviews(
    session: Path, manifest: dict[str, object], records_by_id: dict[str, dict[str, object]]
) -> dict[str, dict[str, object]]:
    path = session / "reviews.jsonl"
    lines = read_jsonl(path)
    expected_metadata = {
        "record_type": "metadata",
        "schema": REVIEW_SCHEMA,
        "session_id": manifest.get("session_id"),
    }
    if not lines or lines[0] != expected_metadata:
        raise ValueError(f"{path}: invalid review journal metadata")
    reviews: dict[str, dict[str, object]] = {}
    for decision in lines[1:]:
        identifier = str(decision.get("id", ""))
        if identifier not in records_by_id:
            raise ValueError(f"{path}: review has unknown id: {identifier or '<missing>'}")
        normalized, _ = validate_decision(records_by_id[identifier], decision)
        if identifier in reviews:
            raise ValueError(f"{path}: duplicate review id: {identifier}")
        reviews[identifier] = normalized
    return reviews


def accelerator_count(text: str) -> int:
    count = 0
    index = 0
    while index < len(text):
        if text[index] != "&":
            index += 1
            continue
        if index + 1 < len(text) and text[index + 1] == "&":
            index += 2
            continue
        if index + 1 < len(text) and not text[index + 1].isspace():
            count += 1
        index += 1
    return count


def resolved_values(
    record: dict[str, object], decision: dict[str, object]
) -> tuple[str | None, list[str] | None]:
    if decision.get("accept_current") is True:
        plurals = record.get("current_plural_translations")
        if plurals:
            return None, list(plurals)
        return str(record.get("current_translation") or ""), None
    if decision.get("accept_source") is True:
        return str(record.get("source", "")), None
    if "translation" in decision:
        return str(decision["translation"]), None
    return None, list(decision["plural_translations"])


def validate_decision(
    record: dict[str, object], decision: dict[str, object]
) -> tuple[dict[str, object], list[str]]:
    identifier = str(decision.get("id", ""))
    unexpected = set(decision) - {"id", *DECISION_FIELDS}
    if unexpected:
        raise ValueError(f"{identifier}: unsupported response fields: {sorted(unexpected)}")
    if identifier != record.get("id"):
        raise ValueError(f"{identifier or '<missing>'}: response id does not match its task record")
    selected: list[str] = []
    for field in DECISION_FIELDS:
        value = decision.get(field)
        if (field.startswith("accept_") and value is True) or (
            not field.startswith("accept_") and value is not None
        ):
            selected.append(field)
    if len(selected) != 1:
        raise ValueError(f"{identifier}: set exactly one reviewed decision field")
    field = selected[0]
    normalized: dict[str, object] = {"id": identifier, field: decision[field]}
    numerus = record.get("numerus") is True
    source = str(record.get("source", ""))
    warnings: list[str] = []
    if field == "accept_current":
        current_values = record.get("current_plural_translations") or [record.get("current_translation")]
        if not current_values or any(not isinstance(value, str) or not value.strip() for value in current_values):
            raise ValueError(f"{identifier}: cannot accept an empty or incomplete current translation")
        if not numerus and current_values == [source]:
            normalized = {"id": identifier, "accept_source": True}
            warnings.append(
                f"{identifier}: normalized source-equal accept_current to accept_source=true"
            )
    elif field == "accept_source":
        if numerus:
            raise ValueError(f"{identifier}: accept_source is supported only for singular messages")
    elif field == "translation":
        value = decision[field]
        if numerus or not isinstance(value, str) or not value.strip():
            raise ValueError(f"{identifier}: translation must be a nonempty singular string")
        if value == source:
            normalized = {"id": identifier, "accept_source": True}
            warnings.append(
                f"{identifier}: normalized source-equal translation to accept_source=true"
            )
    else:
        values = decision[field]
        arity = int(record.get("plural_arity", 0))
        if (
            not numerus
            or not isinstance(values, list)
            or len(values) != arity
            or any(not isinstance(value, str) or not value.strip() for value in values)
        ):
            raise ValueError(
                f"{identifier}: plural_translations must contain exactly {arity} nonempty strings"
            )
        if any(value == source for value in values):
            raise ValueError(f"{identifier}: plural translations must not copy the source")

    singular, plurals = resolved_values(record, normalized)
    values = plurals if plurals is not None else [singular or ""]
    for value in values:
        problems = validate_translation(source, value, allow_missing_placeholders=numerus)
        if problems:
            raise ValueError(f"{identifier}: {'; '.join(problems)}")
        if not numerus and not placeholder_counts_match(source, value):
            raise ValueError(f"{identifier}: placeholder counts do not match the source")
        missing = missing_rich_tags(source, value)
        if missing:
            raise ValueError(f"{identifier}: missing required rich-text tags: {dict(missing)}")
        unbalanced = unbalanced_rich_tags(value)
        if unbalanced:
            raise ValueError(f"{identifier}: unbalanced rich-text tags: {unbalanced}")
        source_accelerators = accelerator_count(source)
        if source_accelerators and accelerator_count(value) != source_accelerators:
            raise ValueError(
                f"{identifier}: accelerator count mismatch: "
                f"source={source_accelerators} translation={accelerator_count(value)}"
            )
        extras = extra_rich_tags(source, value)
        if extras:
            warnings.append(f"{identifier}: additional rich-text tags: {dict(extras)}")
        if source.count("\n") != value.count("\n"):
            warnings.append(
                f"{identifier}: newline count differs: "
                f"source={source.count(chr(10))} translation={value.count(chr(10))}"
            )
    if numerus and not any(placeholder_counts_match(source, value) for value in values):
        raise ValueError(f"{identifier}: no plural form preserves the complete source placeholder set")
    return normalized, warnings


def load_response(
    path: Path,
    manifest: dict[str, object],
    tasks_by_id: dict[str, dict[str, object]],
    records_by_id: dict[str, dict[str, object]],
) -> tuple[str, dict[str, dict[str, object]], list[str]]:
    lines = read_jsonl(path)
    if not lines:
        raise ValueError(f"{path}: response file is empty")
    metadata = lines[0]
    task_id = str(metadata.get("task_id", ""))
    task = tasks_by_id.get(task_id)
    if task is None:
        raise ValueError(f"{path}: response refers to unknown task: {task_id or '<missing>'}")
    expected_metadata = response_metadata(manifest, task_id, task)
    if metadata != expected_metadata:
        raise ValueError(f"{path}: stale or invalid response metadata")
    task_ids = set(task.get("record_ids", []))
    decisions: dict[str, dict[str, object]] = {}
    warnings: list[str] = []
    for decision in lines[1:]:
        identifier = str(decision.get("id", ""))
        if identifier not in task_ids:
            raise ValueError(f"{path}: response id is not owned by {task_id}: {identifier or '<missing>'}")
        if identifier in decisions:
            raise ValueError(f"{path}: duplicate response id: {identifier}")
        normalized, decision_warnings = validate_decision(records_by_id[identifier], decision)
        decisions[identifier] = normalized
        warnings.extend(decision_warnings)
    return task_id, decisions, warnings


def response_metadata(
    manifest: dict[str, object], task_id: str, task: dict[str, object]
) -> dict[str, object]:
    return {
        "record_type": "metadata",
        "schema": RESPONSE_SCHEMA,
        "session_id": manifest.get("session_id"),
        "task_id": task_id,
        "task_sha256": task.get("sha256"),
    }


def write_review_journal(
    path: Path,
    manifest: dict[str, object],
    record_order: Sequence[dict[str, object]],
    reviews: dict[str, dict[str, object]],
) -> None:
    metadata = {
        "record_type": "metadata",
        "schema": REVIEW_SCHEMA,
        "session_id": manifest.get("session_id"),
    }
    ordered = [reviews[str(record["id"])] for record in record_order if str(record["id"]) in reviews]
    atomic_write_text(path, jsonl_text(metadata, ordered))


def catalog_path(manifest: dict[str, object]) -> Path:
    return Path(str(manifest.get("catalog", ""))).resolve()


def command_start(args: argparse.Namespace) -> int:
    catalog = args.catalog.resolve()
    _, messages = parse_catalog(catalog)
    contexts = set(args.context)
    targets: list[CatalogMessage] = []
    for message in messages:
        if message.translation_type in SKIPPED_TYPES or (contexts and message.identity.context not in contexts):
            continue
        values = message.plural_translations if message.plural_translations else [message.translation or ""]
        is_empty = not any(value.strip() for value in values)
        is_source_equal = (
            not message.identity.numerus
            and message.translation is not None
            and message.translation == message.identity.source
        )
        if (
            message.translation_type == "unfinished"
            or (args.include_empty_active and is_empty)
            or (args.include_source_equal and is_source_equal)
        ):
            targets.append(message)

    exact_index, context_index = build_suggestion_indexes(messages)
    records = [
        task_record(
            message,
            build_suggestions(message, exact_index, context_index, args.suggestions, args.similarity),
        )
        for message in targets
    ]
    task_groups = split_tasks(records, args.max_records, args.max_bytes)
    fingerprint = catalog_fingerprint(messages)
    session_id = uuid.uuid4().hex
    repo_root = find_repo_root(catalog.parent)
    timestamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%SZ")
    session = (
        args.session_dir.resolve()
        if args.session_dir
        else repo_root / ".translation-work" / f"{catalog.stem}-{timestamp}-{fingerprint[:8]}-{session_id[:8]}"
    )
    if session.exists():
        raise ValueError(f"{session}: session directory already exists")
    session.parent.mkdir(parents=True, exist_ok=True)
    staging = session.with_name(f".{session.name}.tmp-{session_id[:8]}")
    if staging.exists():
        raise ValueError(f"{staging}: staging directory already exists")

    manifest: dict[str, object] = {
        "schema": SESSION_SCHEMA,
        "session_id": session_id,
        "catalog": str(catalog),
        "catalog_fingerprint": fingerprint,
        "target_count": len(records),
        "selection": {
            "contexts": args.context,
            "include_empty_active": args.include_empty_active,
            "include_source_equal": args.include_source_equal,
            "suggestions": args.suggestions,
            "similarity": args.similarity,
            "max_records": args.max_records,
            "max_bytes": args.max_bytes,
        },
        "tasks": [],
    }
    try:
        (staging / "tasks").mkdir(parents=True)
        (staging / "responses").mkdir()
        for index, group in enumerate(task_groups, 1):
            task_id = f"task-{index:04d}"
            task_relative = f"tasks/{task_id}.jsonl"
            response_relative = f"responses/{task_id}.jsonl"
            task_metadata = {
                "record_type": "metadata",
                "schema": TASK_SCHEMA,
                "session_id": session_id,
                "task_id": task_id,
            }
            task_text = jsonl_text(task_metadata, group)
            task_hash = text_sha256(task_text)
            atomic_write_text(staging / task_relative, task_text)
            initial_response_metadata = {
                "record_type": "metadata",
                "schema": RESPONSE_SCHEMA,
                "session_id": session_id,
                "task_id": task_id,
                "task_sha256": task_hash,
            }
            atomic_write_text(
                staging / response_relative, jsonl_text(initial_response_metadata, [])
            )
            manifest["tasks"].append(
                {
                    "id": task_id,
                    "path": task_relative,
                    "response_path": response_relative,
                    "sha256": task_hash,
                    "record_ids": [record["id"] for record in group],
                }
            )
        review_metadata = {
            "record_type": "metadata",
            "schema": REVIEW_SCHEMA,
            "session_id": session_id,
        }
        atomic_write_text(staging / "reviews.jsonl", jsonl_text(review_metadata, []))
        atomic_write_text(
            staging / "manifest.json",
            json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        )
        os.replace(staging, session)
    finally:
        if staging.exists():
            shutil.rmtree(staging)

    print(f"Created translation session with {len(records)} message(s) in {len(task_groups)} task(s).")
    print(session)
    return 0


def command_check(args: argparse.Namespace) -> int:
    session, manifest, records, tasks_by_id = load_session(args.session)
    records_by_id = {str(record["id"]): record for record in records}
    task_id, decisions, warnings = load_response(args.response.resolve(), manifest, tasks_by_id, records_by_id)
    expected = len(tasks_by_id[task_id]["record_ids"])
    if args.require_complete_task and len(decisions) != expected:
        raise ValueError(f"{args.response}: {task_id} has {len(decisions)}/{expected} reviewed record(s)")
    for warning in warnings:
        print(f"WARNING: {warning}")
    print(f"Valid response for {task_id}: {len(decisions)}/{expected} reviewed record(s).")
    return 0


def command_salvage(args: argparse.Namespace) -> int:
    session, manifest, records, tasks_by_id = load_session(args.session)
    records_by_id = {str(record["id"]): record for record in records}
    response = args.response.resolve()
    task_id = ""
    task: dict[str, object] | None = None
    for candidate_id, candidate in tasks_by_id.items():
        if session_file(session, candidate["response_path"]) == response:
            task_id = candidate_id
            task = candidate
            break
    if task is None:
        raise ValueError(f"{response}: response is not owned by this session")

    task_ids = set(task["record_ids"])
    decisions: dict[str, dict[str, object]] = {}
    rejected: list[str] = []
    warnings: list[str] = []
    with response.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as error:
                rejected.append(f"line {line_number}: invalid JSON: {error}")
                continue
            if not isinstance(item, dict):
                rejected.append(f"line {line_number}: JSONL record is not an object")
                continue
            if item.get("record_type") == "metadata":
                continue
            identifier = str(item.get("id", ""))
            if identifier not in task_ids:
                rejected.append(
                    f"line {line_number}: response id is not owned by {task_id}: "
                    f"{identifier or '<missing>'}"
                )
                continue
            try:
                normalized, decision_warnings = validate_decision(records_by_id[identifier], item)
            except ValueError as error:
                rejected.append(f"line {line_number}: {error}")
                continue
            existing = decisions.get(identifier)
            if existing is not None and existing != normalized:
                rejected.append(f"line {line_number}: conflicting duplicate response id: {identifier}")
                continue
            decisions[identifier] = normalized
            warnings.extend(decision_warnings)

    ordered = [
        decisions[str(record["id"])]
        for record in task["records"]
        if str(record["id"]) in decisions
    ]
    if args.write:
        atomic_write_text(response, jsonl_text(response_metadata(manifest, task_id, task), ordered))
    for warning in warnings:
        print(f"WARNING: {warning}")
    for problem in rejected:
        print(f"SKIPPED: {problem}")
    action = "Salvaged" if args.write else "Would salvage"
    print(
        f"{action} {len(ordered)}/{len(task_ids)} valid decision(s) for {task_id}; "
        f"skipped {len(rejected)} invalid line(s)."
    )
    if not args.write:
        print("Dry-run only; pass --write to rebuild the response atomically.")
    return 0


def response_paths(
    session: Path,
    manifest: dict[str, object],
    provided: Sequence[Path],
) -> list[Path]:
    if provided:
        return [path.resolve() for path in provided]
    return [session_file(session, task["response_path"]) for task in manifest.get("tasks", [])]


def command_merge(args: argparse.Namespace) -> int:
    session, manifest, records, tasks_by_id = load_session(args.session)
    records_by_id = {str(record["id"]): record for record in records}
    reviews = load_reviews(session, manifest, records_by_id)
    merged = dict(reviews)
    incoming_count = 0
    warnings: list[str] = []
    for path in response_paths(session, manifest, args.responses):
        _, decisions, response_warnings = load_response(path, manifest, tasks_by_id, records_by_id)
        warnings.extend(response_warnings)
        for identifier, decision in decisions.items():
            existing = merged.get(identifier)
            if existing is not None and existing != decision and not args.replace:
                raise ValueError(
                    f"{identifier}: response conflicts with the merged review; use --replace to correct it"
                )
            if existing != decision:
                incoming_count += 1
            merged[identifier] = decision
    if args.write:
        write_review_journal(session / "reviews.jsonl", manifest, records, merged)
    for warning in warnings:
        print(f"WARNING: {warning}")
    action = "Merged" if args.write else "Would merge"
    print(f"{action} {incoming_count} new or changed review(s); total={len(merged)}/{len(records)}.")
    if not args.write:
        print("Dry-run only; pass --write to update the review journal.")
    return 0


def catalog_conflicts(
    manifest: dict[str, object],
    records: Sequence[dict[str, object]],
    reviews: dict[str, dict[str, object]],
) -> tuple[bool, list[str]]:
    catalog = catalog_path(manifest)
    _, messages = parse_catalog(catalog)
    if catalog_fingerprint(messages) != manifest.get("catalog_fingerprint"):
        return True, ["catalog source identity has changed since the session started"]
    messages_by_id = {message.identifier: message for message in messages}
    conflicts: list[str] = []
    for record in records:
        identifier = str(record["id"])
        message = messages_by_id.get(identifier)
        if message is None:
            conflicts.append(f"{identifier}: target message is missing")
            continue
        current = state_for_message(message)
        if current == state_for_record(record):
            continue
        decision = reviews.get(identifier)
        if decision is not None:
            singular, plurals = resolved_values(record, decision)
            desired_values = plurals if plurals is not None else [singular or ""]
            current_values = (
                message.plural_translations
                if message.plural_translations
                else [message.translation or ""]
            )
            if message.translation_type == "finished" and current_values == desired_values:
                continue
        conflicts.append(f"{identifier}: targeted translation changed outside this session")
    return bool(conflicts), conflicts


def command_status(args: argparse.Namespace) -> int:
    session, manifest, records, tasks_by_id = load_session(args.session)
    records_by_id = {str(record["id"]): record for record in records}
    reviews = load_reviews(session, manifest, records_by_id)
    invalid_responses: list[str] = []
    invalid_response_paths: list[str] = []
    unmerged_ids: set[str] = set()
    response_decisions_by_task: dict[str, dict[str, dict[str, object]]] = {}
    for path in response_paths(session, manifest, []):
        try:
            task_id, decisions, _ = load_response(path, manifest, tasks_by_id, records_by_id)
        except (OSError, ValueError) as error:
            invalid_responses.append(str(error))
            invalid_response_paths.append(str(path))
            continue
        response_decisions_by_task[task_id] = decisions
        unmerged_ids.update(
            identifier for identifier, decision in decisions.items() if reviews.get(identifier) != decision
        )
    stale, conflicts = catalog_conflicts(manifest, records, reviews)
    task_status: list[dict[str, object]] = []
    for task_id, task in tasks_by_id.items():
        ids = set(task["record_ids"])
        merged_count = len(ids & reviews.keys())
        checkpointed = response_decisions_by_task.get(task_id, {})
        task_unmerged = {
            identifier
            for identifier, decision in checkpointed.items()
            if reviews.get(identifier) != decision
        }
        task_status.append(
            {
                "id": task_id,
                "merged": merged_count,
                "checkpointed": len(checkpointed),
                "unmerged": len(task_unmerged),
                "total": len(ids),
                "remaining": len(ids) - merged_count,
                "task": str(session_file(session, task["path"])),
                "response": str(session_file(session, task["response_path"])),
            }
        )
    result = {
        "session": str(session),
        "catalog": str(catalog_path(manifest)),
        "reviewed": len(reviews),
        "total": len(records),
        "remaining": len(records) - len(reviews),
        "unmerged": len(unmerged_ids),
        "catalog_stale_or_conflicted": stale,
        "conflicts": conflicts,
        "invalid_responses": invalid_responses,
        "invalid_response_paths": invalid_response_paths,
        "tasks": task_status,
    }
    if stale:
        result["queue_complete"] = False
        result["recommended_action"] = "resolve_errors"
    elif invalid_responses:
        result["queue_complete"] = False
        result["recommended_action"] = "salvage_responses"
    elif unmerged_ids:
        result["queue_complete"] = False
        result["recommended_action"] = "merge_responses"
    elif result["remaining"]:
        result["queue_complete"] = False
        result["recommended_action"] = "dispatch_workers"
    else:
        result["queue_complete"] = True
        result["recommended_action"] = "dry_run_apply"
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    else:
        print(
            f"Session status: reviewed={result['reviewed']}, remaining={result['remaining']}, "
            f"unmerged={result['unmerged']}, invalid_responses={len(invalid_responses)}."
        )
        for item in task_status:
            if item["remaining"]:
                print(
                    f"{item['id']}: {item['merged']}/{item['total']} merged, "
                    f"{item['checkpointed']} checkpointed, {item['unmerged']} unmerged; "
                    f"task={item['task']} response={item['response']}"
                )
        for problem in conflicts:
            print(f"ERROR: {problem}")
        for problem in invalid_responses:
            print(f"ERROR: {problem}")
        for path in invalid_response_paths:
            print(
                "RECOVERY: "
                f"python3 scripts/translation/translate_ts.py salvage {session} {path} --write"
            )
        if not stale and not invalid_responses:
            if unmerged_ids:
                print(
                    "INCOMPLETE: merge the checkpointed responses, then continue the rolling worker queue. "
                    "Do not stop at a partial session."
                )
            elif result["remaining"]:
                print(
                    "INCOMPLETE: dispatch the next bounded tasks and continue across worker waves until "
                    "remaining=0. Concurrent agent slots are not a total-capacity limit."
                )
            else:
                print("REVIEW QUEUE COMPLETE: proceed with dry-run apply, write, validation, and diff inspection.")
    return 1 if stale or invalid_responses else 0


def command_apply(args: argparse.Namespace) -> int:
    session, manifest, records, _ = load_session(args.session)
    records_by_id = {str(record["id"]): record for record in records}
    reviews = load_reviews(session, manifest, records_by_id)
    missing = [str(record["id"]) for record in records if str(record["id"]) not in reviews]
    if missing and not args.reviewed_only:
        raise ValueError(f"session is incomplete: {len(missing)} record(s) remain unreviewed")
    selected_records = (
        [record for record in records if str(record["id"]) in reviews]
        if args.reviewed_only
        else records
    )
    if not selected_records:
        raise ValueError("session has no reviewed translations to apply")
    for record in selected_records:
        validate_decision(record, reviews[str(record["id"])])

    catalog = catalog_path(manifest)
    _, messages = parse_catalog(catalog)
    if catalog_fingerprint(messages) != manifest.get("catalog_fingerprint"):
        raise ValueError("catalog source identity has changed since the session started")
    messages_by_id = {message.identifier: message for message in messages}
    with catalog.open("r", encoding="utf-8", newline="") as stream:
        original = stream.read()
    spans = scan_raw_messages(original)
    replacements: list[tuple[int, int, str]] = []
    already_applied = 0
    for record in selected_records:
        identifier = str(record["id"])
        message = messages_by_id.get(identifier)
        span = spans.get(identifier)
        if message is None or span is None:
            raise ValueError(f"{identifier}: target message no longer exists")
        singular, plurals = resolved_values(record, reviews[identifier])
        desired_values = plurals if plurals is not None else [singular or ""]
        current_values = (
            message.plural_translations if message.plural_translations else [message.translation or ""]
        )
        if message.translation_type == "finished" and current_values == desired_values:
            already_applied += 1
            continue
        if state_for_message(message) != state_for_record(record):
            raise ValueError(f"{identifier}: targeted translation changed outside this session")
        replacement = replace_translation_node(span.block, singular, plurals)
        replacements.append((span.start, span.end, replacement))

    updated = original
    for start, end, replacement in sorted(replacements, reverse=True):
        updated = updated[:start] + replacement + updated[end:]
    try:
        staged_messages = list(iter_catalog_messages(ET.fromstring(updated)))
    except ET.ParseError as error:
        raise ValueError(f"staged catalog is not well-formed XML: {error}") from error
    staged_by_id = {message.identifier: message for message in staged_messages}
    for record in selected_records:
        identifier = str(record["id"])
        message = staged_by_id.get(identifier)
        if message is None or message.translation_type != "finished":
            raise ValueError(f"{identifier}: staged translation is missing or unfinished")
        singular, plurals = resolved_values(record, reviews[identifier])
        expected = plurals if plurals is not None else [singular or ""]
        actual = message.plural_translations if message.plural_translations else [message.translation or ""]
        if actual != expected:
            raise ValueError(f"{identifier}: staged translation differs from its reviewed value")
    if args.write and replacements:
        atomic_write_text(catalog, updated, mode=catalog.stat().st_mode)
    action = "Applied" if args.write else "Would apply"
    print(
        f"{action} {len(replacements)} translation(s); "
        f"{already_applied} already matched the reviewed session"
        f"{f'; {len(missing)} remain unreviewed' if args.reviewed_only else ''}."
    )
    if not args.write:
        print("Dry-run only; pass --write to update the catalog.")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    start = subparsers.add_parser("start", help="create a new resumable translation session")
    start.add_argument("catalog", type=Path)
    start.add_argument("--session-dir", type=Path)
    start.add_argument("--context", action="append", default=[])
    start.add_argument("--include-empty-active", action="store_true")
    start.add_argument("--include-source-equal", action="store_true")
    start.add_argument("--suggestions", type=int, default=2)
    start.add_argument("--similarity", type=float, default=0.72)
    start.add_argument("--max-records", type=int, default=DEFAULT_MAX_RECORDS)
    start.add_argument("--max-bytes", type=int, default=DEFAULT_MAX_BYTES)
    start.set_defaults(function=command_start)

    status = subparsers.add_parser("status", help="show durable and unmerged session progress")
    status.add_argument("session", type=Path)
    status.add_argument("--json", action="store_true")
    status.set_defaults(function=command_status)

    check = subparsers.add_parser("check", help="validate one agent response file")
    check.add_argument("session", type=Path)
    check.add_argument("response", type=Path)
    check.add_argument("--require-complete-task", action="store_true")
    check.set_defaults(function=command_check)

    salvage = subparsers.add_parser(
        "salvage", help="atomically rebuild a damaged response from its valid decisions"
    )
    salvage.add_argument("session", type=Path)
    salvage.add_argument("response", type=Path)
    salvage.add_argument("--write", action="store_true")
    salvage.set_defaults(function=command_salvage)

    merge = subparsers.add_parser("merge", help="merge response checkpoints into the review journal")
    merge.add_argument("session", type=Path)
    merge.add_argument("responses", nargs="*", type=Path)
    merge.add_argument("--write", action="store_true")
    merge.add_argument("--replace", action="store_true")
    merge.set_defaults(function=command_merge)

    apply = subparsers.add_parser(
        "apply", help="atomically apply complete or explicitly selected reviewed translations"
    )
    apply.add_argument("session", type=Path)
    apply.add_argument(
        "--reviewed-only",
        action="store_true",
        help="apply only checkpointed reviews when explicitly abandoning or restarting a session",
    )
    apply.add_argument("--write", action="store_true")
    apply.set_defaults(function=command_apply)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.command == "start":
        if args.max_records < 1:
            raise SystemExit("--max-records must be at least 1")
        if args.max_bytes < 1:
            raise SystemExit("--max-bytes must be at least 1")
        if args.suggestions < 0:
            raise SystemExit("--suggestions cannot be negative")
        if not 0.0 <= args.similarity <= 1.0:
            raise SystemExit("--similarity must be between 0 and 1")
    try:
        return int(args.function(args))
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
