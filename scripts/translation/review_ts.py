#!/usr/bin/env python3
"""Checkpoint semantic Qt TS audits and bind approvals to the exact reviewed translations."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import uuid
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from translation.translate_ts import (  # noqa: E402
    find_repo_root, read_json, read_jsonl, session_file, split_tasks, validate_decision,
)
from translation.ts_utils import (  # noqa: E402
    SKIPPED_TYPES, atomic_write_text, jsonl_text, load_jsonl, message_to_task_record,
    parse_catalog_text, text_sha256,
)

SCHEMA = "duckstation-semantic-review-v1"


def record_digest(record: dict) -> str:
    fields = ("id", "current_type", "current_translation", "current_plural_translations", "translator_comment")
    return text_sha256(json.dumps({key: record.get(key, "" if key == "translator_comment" else None)
                                  for key in fields}, ensure_ascii=False, sort_keys=True))


def metadata(manifest: dict, task: dict | None = None) -> dict:
    result = {"record_type": "metadata", "schema": SCHEMA, "session_id": manifest["session_id"]}
    if task is not None:
        result.update({"task_id": task["id"], "task_sha256": task["sha256"]})
    return result


def load_session(session: Path) -> tuple[Path, dict, list[dict], dict[str, dict]]:
    session = session.resolve()
    manifest = read_json(session / "manifest.json")
    if manifest.get("schema") != SCHEMA:
        raise ValueError("unsupported semantic review schema")
    snapshot = read_text(session / "snapshot.ts")
    if text_sha256(snapshot) != manifest["snapshot_sha256"]:
        raise ValueError("review snapshot checksum mismatch")
    if manifest.get("guidance_sha256") and text_sha256(read_text(session / "guidance.json")) != manifest["guidance_sha256"]:
        raise ValueError("review guidance checksum mismatch")
    records, tasks, seen = [], {}, set()
    for task in manifest["tasks"]:
        task_path = session_file(session, task["path"])
        if text_sha256(read_text(task_path)) != task["sha256"]:
            raise ValueError("immutable review task checksum mismatch")
        lines = read_jsonl(task_path)
        expected = {**metadata(manifest), "task_id": task["id"], "language": manifest["catalog_attributes"].get("language", ""),
                    "catalog": manifest["catalog"], "repo_root": manifest["repo_root"], "guidance": manifest.get("guidance_path")}
        if not lines or lines[0] != expected or [record.get("id") for record in lines[1:]] != task["record_ids"]:
            raise ValueError("review task metadata or ownership mismatch")
        if task["id"] in tasks:
            raise ValueError("duplicate review task")
        for record in lines[1:]:
            if record.get("record_type") != "message" or record["id"] in seen:
                raise ValueError("invalid or duplicate review message")
            seen.add(record["id"])
        tasks[task["id"]] = {**task, "records": lines[1:]}
        records.extend(lines[1:])
    if len(records) != manifest["target_count"]:
        raise ValueError("review task coverage does not match manifest")
    return session, manifest, records, tasks


def read_text(path: Path) -> str:
    with path.open(encoding="utf-8", newline="") as stream:
        return stream.read()


def validate_result(record: dict, result: dict) -> dict:
    if not isinstance(result, dict):
        raise ValueError("semantic review results must be JSON objects")
    if set(result) - {"id", "outcome", "reason", "source_reason", "findings", "reviewer"}:
        raise ValueError("unsupported semantic review fields")
    if result.get("id") != record["id"] or result.get("outcome") not in ("pass", "finding", "needs_context"):
        raise ValueError("invalid review ID or outcome")
    if not isinstance(result.get("reviewer"), str) or not result["reviewer"].strip():
        raise ValueError("each result must name its reviewer")
    if result["outcome"] == "pass":
        if result.get("findings"):
            raise ValueError("a passing review cannot contain findings")
        validate_decision(record, {"id": record["id"], "accept_current": True})
        values = record.get("current_plural_translations") or [record.get("current_translation") or ""]
        if (any(value == record["source"] for value in values)
                and (not isinstance(result.get("source_reason"), str) or not result["source_reason"].strip())):
            raise ValueError("source-equal text requires the reviewer's explicit source_reason")
    elif result["outcome"] == "needs_context":
        if not isinstance(result.get("reason"), str) or not result["reason"].strip():
            raise ValueError("needs_context requires a concrete question")
    else:
        findings = result.get("findings")
        if not isinstance(findings, list) or not findings:
            raise ValueError("finding outcome requires findings")
        for finding in findings:
            if not isinstance(finding, dict) or set(finding) - {"severity", "category", "reason", "translation", "plural_translations"}:
                raise ValueError("unsupported finding fields")
            if finding.get("severity") not in ("high", "medium", "low") or finding.get("category") not in ("accuracy", "fluency", "terminology", "format"):
                raise ValueError("invalid finding severity or category")
            if not isinstance(finding.get("reason"), str) or not finding["reason"].strip():
                raise ValueError("finding must explain the concrete defect")
            replacement = {"id": record["id"], **{key: finding[key] for key in ("translation", "plural_translations") if key in finding}}
            validate_decision(record, replacement)
    return dict(result)


def load_journal(session: Path, manifest: dict, records: list[dict]) -> dict[str, dict]:
    lines = read_jsonl(session / "reviews.jsonl")
    if not lines or lines[0] != metadata(manifest):
        raise ValueError("invalid semantic review journal metadata")
    by_id = {record["id"]: record for record in records}
    results = {}
    for item in lines[1:]:
        identifier = item.get("id")
        if identifier not in by_id or identifier in results:
            raise ValueError("unknown or duplicate journal ID")
        results[identifier] = validate_result(by_id[identifier], item)
    return results


def start_session(catalog: Path, session_dir: Path | None = None, identifiers: set[str] | None = None,
                  previous: Path | None = None, guidance: str | None = None,
                  max_records: int = 150, max_bytes: int = 192 * 1024) -> Path:
    catalog = catalog.resolve()
    snapshot = read_text(catalog)
    tree, messages = parse_catalog_text(snapshot)
    records = [message_to_task_record(message) for message in messages
               if message.translation_type not in SKIPPED_TYPES and (identifiers is None or message.identifier in identifiers)]
    if identifiers is not None and {record["id"] for record in records} != identifiers:
        raise ValueError("requested review IDs are missing or inactive")
    language = tree.getroot().get("language", "")
    if not language:
        raise ValueError("review catalog must declare its language")
    for record in records:
        record["language"] = language
    identifier = uuid.uuid4().hex
    repo_root = find_repo_root(catalog.parent)
    session = (session_dir or repo_root / ".translation-work" / f"audit-{catalog.stem}-{identifier[:12]}").resolve()
    if session.exists():
        raise ValueError("review session already exists; use status to resume")
    guide_text = read_text(Path(guidance)) if guidance else None
    if guide_text is not None and json.loads(guide_text).get("language") != language:
        raise ValueError("review guidance language does not match catalog")
    manifest = {"schema": SCHEMA, "session_id": identifier, "catalog": str(catalog), "repo_root": str(repo_root),
                "catalog_attributes": dict(tree.getroot().attrib), "snapshot_sha256": text_sha256(snapshot),
                "target_count": len(records), "tasks": [], "previous": str(previous) if previous else None,
                "guidance_path": str(session / "guidance.json") if guide_text is not None else None,
                "guidance_sha256": text_sha256(guide_text) if guide_text is not None else None}
    retained = {}
    if previous:
        old_session, old_manifest, old_records, _ = load_session(previous)
        old_results = load_journal(old_session, old_manifest, old_records)
        old_by_id = {record["id"]: record for record in old_records}
        if old_manifest["catalog_attributes"] == manifest["catalog_attributes"] and old_manifest.get("guidance_sha256") == manifest["guidance_sha256"]:
            retained = {record["id"]: old_results[record["id"]] for record in records
                        if record["id"] in old_results and record_digest(record) == record_digest(old_by_id[record["id"]])}
    staging = session.with_name(f".{session.name}.tmp-{identifier[:8]}")
    staging.mkdir(parents=True)
    try:
        atomic_write_text(staging / "snapshot.ts", snapshot)
        if guide_text is not None:
            atomic_write_text(staging / "guidance.json", guide_text)
        for index, group in enumerate(split_tasks(records, max_records, max_bytes), 1):
            task_id = f"task-{index:04d}"
            task = {"id": task_id, "path": f"tasks/{task_id}.jsonl", "response_path": f"responses/{task_id}.jsonl",
                    "record_ids": [record["id"] for record in group]}
            header = {**metadata(manifest), "task_id": task_id, "language": language,
                      "catalog": str(catalog), "repo_root": str(repo_root), "guidance": manifest["guidance_path"]}
            task_text = jsonl_text(header, group)
            if len(task_text.encode("utf-8")) > max_bytes:
                raise ValueError("review task exceeds maximum bytes")
            task["sha256"] = text_sha256(task_text)
            atomic_write_text(staging / task["path"], task_text)
            atomic_write_text(staging / task["response_path"], jsonl_text(metadata(manifest, task), []))
            manifest["tasks"].append(task)
        atomic_write_text(staging / "reviews.jsonl", jsonl_text(metadata(manifest), list(retained.values())))
        atomic_write_text(staging / "manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        os.replace(staging, session)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return session


def load_response(path: Path, manifest: dict, tasks: dict) -> dict[str, dict]:
    lines = read_jsonl(path)
    task = tasks.get(lines[0].get("task_id")) if lines else None
    if task is None or lines[0] != metadata(manifest, task):
        raise ValueError("invalid or stale semantic response metadata")
    by_id = {record["id"]: record for record in task["records"]}
    results = {}
    for item in lines[1:]:
        if item.get("id") not in by_id or item["id"] in results:
            raise ValueError("unknown, unowned, or duplicate response ID")
        results[item["id"]] = validate_result(by_id[item["id"]], item)
    return results


def require_approved(session: Path, attributes: dict, records: list[dict]) -> None:
    session, manifest, reviewed_records, _ = load_session(session)
    results = load_journal(session, manifest, reviewed_records)
    by_id = {record["id"]: record for record in reviewed_records}
    if manifest["catalog_attributes"] != attributes:
        raise ValueError("semantic review language/version does not match")
    for record in records:
        identifier = record["id"]
        if (identifier not in by_id or record_digest(record) != record_digest(by_id[identifier])
                or results.get(identifier, {}).get("outcome") != "pass"):
            raise ValueError(f"{identifier}: exact translation still needs semantic approval")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    start = commands.add_parser("start")
    start.add_argument("catalog", type=Path)
    start.add_argument("--session-dir", type=Path)
    start.add_argument("--records", nargs="+", type=Path)
    start.add_argument("--previous", type=Path)
    start.add_argument("--guidance")
    start.add_argument("--max-records", type=int, default=150)
    start.add_argument("--max-bytes", type=int, default=192 * 1024)
    for name in ("status", "check", "merge", "salvage"):
        command = commands.add_parser(name)
        command.add_argument("session", type=Path)
        if name in {"check", "salvage"}:
            command.add_argument("response", type=Path)
        if name == "merge":
            command.add_argument("responses", nargs="*", type=Path)
            command.add_argument("--replace", action="store_true")
        if name in {"merge", "salvage"}:
            command.add_argument("--write", action="store_true")
        if name == "status":
            command.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "start":
            if args.max_records < 1 or args.max_bytes < 1:
                raise ValueError("task bounds must be positive")
            identifiers = {record["id"] for record in load_jsonl(args.records)[1]} if args.records else None
            print(start_session(args.catalog, args.session_dir, identifiers, args.previous, args.guidance, args.max_records, args.max_bytes))
            return 0
        session, manifest, records, tasks = load_session(args.session)
        results = load_journal(session, manifest, records)
        if args.command == "salvage":
            task = next((task for task in tasks.values() if session_file(session, task["response_path"]) == args.response.resolve()), None)
            if task is None:
                raise ValueError("response is not owned by this review session")
            by_id = {record["id"]: record for record in task["records"]}
            recovered, skipped = {}, 0
            original = args.response.read_bytes()
            for raw_line in original.splitlines():
                try:
                    line = raw_line.decode("utf-8")
                    if not line.strip():
                        continue
                    item = json.loads(line)
                    if isinstance(item, dict) and item.get("record_type") == "metadata":
                        continue
                    item = validate_result(by_id[item["id"]], item)
                    if item["id"] in recovered and recovered[item["id"]] != item:
                        raise ValueError("conflicting duplicate result")
                    recovered[item["id"]] = item
                except (ValueError, KeyError, TypeError):
                    skipped += 1
            if args.write:
                ordered = [recovered[record["id"]] for record in task["records"] if record["id"] in recovered]
                atomic_write_text(args.response, jsonl_text(metadata(manifest, task), ordered),
                                  expected_sha256=hashlib.sha256(original).hexdigest())
            print(f"{'Salvaged' if args.write else 'Would salvage'} {len(recovered)} result(s); skipped {skipped} invalid line(s).")
            return 0
        if args.command == "check":
            incoming = load_response(args.response, manifest, tasks)
            print(f"Valid semantic response: {len(incoming)} result(s).")
            return 0
        stale = text_sha256(read_text(Path(manifest["catalog"]))) != manifest["snapshot_sha256"]
        if args.command == "merge":
            if stale:
                raise ValueError("catalog changed; start a fresh review with --previous SESSION to retain unchanged results")
            paths = args.responses or [session_file(session, task["response_path"]) for task in tasks.values()]
            for path in paths:
                for identifier, item in load_response(path, manifest, tasks).items():
                    if identifier in results and results[identifier] != item and not args.replace:
                        raise ValueError("review result conflicts with journal; use --replace deliberately")
                    results[identifier] = item
            if args.write:
                atomic_write_text(session / "reviews.jsonl", jsonl_text(metadata(manifest),
                                  [results[record["id"]] for record in records if record["id"] in results]))
            print(f"{'Merged' if args.write else 'Would merge'} {len(results)}/{len(records)} reviewed message(s).")
            return 0
        invalid, task_status = [], []
        for task in tasks.values():
            try:
                checkpoint = load_response(session_file(session, task["response_path"]), manifest, tasks)
            except (OSError, ValueError) as error:
                invalid.append(str(error))
                checkpoint = {}
            task_status.append({"id": task["id"], "remaining_ids": [key for key in task["record_ids"] if key not in results],
                                "unmerged": sum(results.get(key) != value for key, value in checkpoint.items()),
                                "task": str(session_file(session, task["path"])),
                                "response": str(session_file(session, task["response_path"]))})
        payload = {"reviewed": len(results), "total": len(records), "remaining": len(records) - len(results),
                   "findings": sum(item["outcome"] == "finding" for item in results.values()),
                   "needs_context": sum(item["outcome"] == "needs_context" for item in results.values()),
                   "stale": stale, "invalid_responses": invalid, "tasks": task_status}
        print(json.dumps(payload, ensure_ascii=False, indent=2) if args.json else
              f"Reviewed {payload['reviewed']}/{payload['total']}; findings={payload['findings']}, "
              f"needs_context={payload['needs_context']}, stale={stale}.\n" +
              "\n".join(f"{item['id']}: task={item['task']} response={item['response']}" for item in task_status if item["remaining_ids"] or item["unmerged"]))
        return int(stale or bool(invalid))
    except (OSError, ValueError, KeyError) as error:
        print(f"ERROR: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
