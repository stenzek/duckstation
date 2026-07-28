#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parents[2]
REPO_ROOT = SCRIPTS_DIR.parent
TRANSLATE = SCRIPTS_DIR / "translation" / "translate_ts.py"
sys.path.insert(0, str(SCRIPTS_DIR))

from translation.translate_ts import validate_decision  # noqa: E402
from translation.ts_utils import (  # noqa: E402
    TRANSLATION_RE,
    catalog_fingerprint,
    extract_placeholders,
    extract_rich_tags,
    parse_catalog,
    placeholder_counts_match,
    placeholders_are_subset,
    placeholders_match,
    replace_translation_node,
    unbalanced_rich_tags,
    validate_translation,
)


FIXTURE = """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="ja">
<context>
    <name>Alpha</name>
    <message>
        <location filename="alpha.cpp" line="1"/>
        <source>Hello %1 {0} ${title} %.1f &lt;strong&gt;world&lt;/strong&gt;</source>
        <translation type="unfinished"></translation>
    </message>
    <message>
        <source>Register</source>
        <translation type="unfinished">レジスタ</translation>
    </message>
    <message numerus="yes">
        <source>%n file(s)</source>
        <comment>File count</comment>
        <translation type="unfinished">
            <numerusform></numerusform>
        </translation>
    </message>
    <message>
        <source>New wording</source>
        <translation type="unfinished"></translation>
    </message>
    <message>
        <source>&amp;Open</source>
        <translation type="unfinished"></translation>
    </message>
    <message>
        <source>Old wording</source>
        <translation type="vanished">古い文言</translation>
    </message>
    <message>
        <source>Hello</source>
        <translation>こんにちは</translation>
    </message>
</context>
<context>
    <name>Beta</name>
    <message>
        <source>Register</source>
        <translation type="unfinished"></translation>
    </message>
    <message>
        <source>Removed</source>
        <translation type="obsolete">削除済み</translation>
    </message>
</context>
</TS>
"""


class TranslationToolTests(unittest.TestCase):
    def run_tool(self, *arguments: object, expect: int = 0) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [sys.executable, str(TRANSLATE), *(str(value) for value in arguments)],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(expect, result.returncode, result.stdout)
        return result

    def run_validator(self, *arguments: object, expect: int = 0) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPTS_DIR / "translation" / "validate_ts.py"),
                *(str(value) for value in arguments),
            ],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(expect, result.returncode, result.stdout)
        return result

    def start_session(
        self, root: Path, fixture: str = FIXTURE, *extra: object
    ) -> tuple[Path, Path, dict[str, object]]:
        catalog = root / "catalog.ts"
        session = root / "session"
        catalog.write_text(fixture, encoding="utf-8")
        self.run_tool("start", catalog, "--session-dir", session, *extra)
        manifest = json.loads((session / "manifest.json").read_text(encoding="utf-8"))
        return catalog, session, manifest

    def task_records(self, session: Path, task: dict[str, object]) -> list[dict[str, object]]:
        lines = [
            json.loads(line)
            for line in (session / str(task["path"])).read_text(encoding="utf-8").splitlines()
        ]
        return lines[1:]

    def response_path(self, session: Path, task: dict[str, object]) -> Path:
        return session / str(task["response_path"])

    def append_decisions(
        self, response: Path, decisions: list[dict[str, object]], malformed_tail: str = ""
    ) -> None:
        text = response.read_text(encoding="utf-8")
        text += "".join(json.dumps(item, ensure_ascii=False, sort_keys=True) + "\n" for item in decisions)
        response.write_text(text + malformed_tail, encoding="utf-8")

    def decision_for(self, record: dict[str, object]) -> dict[str, object]:
        identifier = record["id"]
        source = record["source"]
        context = record["context"]
        if source == "Hello %1 {0} ${title} %.1f <strong>world</strong>":
            return {
                "id": identifier,
                "translation": "こんにちは %1 {0} ${title} %.1f <strong>世界</strong>",
            }
        if source == "%n file(s)":
            return {"id": identifier, "plural_translations": ["%n ファイル"]}
        if source == "New wording":
            return {"id": identifier, "translation": "新しい文言"}
        if source == "&Open":
            return {"id": identifier, "translation": "開く(&O)"}
        if source == "Register" and context == "Alpha":
            return {"id": identifier, "translation": "登録"}
        if source == "Register":
            return {"id": identifier, "translation": "登録"}
        raise AssertionError((context, source))

    def complete_responses(self, session: Path, manifest: dict[str, object]) -> None:
        for task in manifest["tasks"]:
            decisions = [self.decision_for(record) for record in self.task_records(session, task)]
            self.append_decisions(self.response_path(session, task), decisions)

    def test_placeholder_recognition_and_compatibility(self) -> None:
        text = "At 100% speed, 5% of users: %1 %n {} {0:08X} %.1f %s ${title} {{}} %%"
        placeholders = extract_placeholders(text)
        self.assertEqual(1, placeholders["qt:%1"])
        self.assertEqual(1, placeholders["qt:%n"])
        self.assertEqual(1, placeholders["fmt:{}"])
        self.assertEqual(1, placeholders["fmt:{0}"])
        self.assertEqual(1, placeholders["printf:%.1f"])
        self.assertEqual(1, placeholders["printf:%s"])
        self.assertEqual(1, placeholders["template:${title}"])
        self.assertEqual(7, sum(placeholders.values()))
        self.assertTrue(placeholders_match("{} {}", "{1} {0}"))
        self.assertFalse(placeholders_match("{} {}", "{0} {0}"))
        self.assertTrue(placeholders_match("Use {0}, then use {0} again", "Usar {0}"))
        self.assertFalse(placeholder_counts_match("Use {0}, then use {0} again", "Usar {0}"))
        self.assertTrue(placeholders_are_subset("{} of %n", "%n"))
        self.assertFalse(placeholders_are_subset("{} of %n", "%n %1"))

    def test_rich_text_helpers_ignore_labels_and_find_unbalanced_tags(self) -> None:
        self.assertFalse(extract_rich_tags("<Parent Directory>"))
        self.assertEqual({"strong": 1, "/strong": 1}, dict(extract_rich_tags("<strong>Text</strong>")))
        self.assertFalse(validate_translation("<strong>Text</strong>", "テキスト"))
        self.assertFalse(unbalanced_rich_tags("<html><head/><body><br><hr/></body></html>"))
        self.assertEqual({"p": (2, 1), "strong": (1, 0)}, unbalanced_rich_tags("<p><p><strong>Text</p>"))

    def test_fingerprint_ignores_translation_changes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first.ts"
            second = root / "second.ts"
            first.write_text(FIXTURE, encoding="utf-8")
            second.write_text(FIXTURE.replace("レジスタ", "登録"), encoding="utf-8")
            _, first_messages = parse_catalog(first)
            _, second_messages = parse_catalog(second)
            self.assertEqual(catalog_fingerprint(first_messages), catalog_fingerprint(second_messages))

    def test_translation_replacement_preserves_source_and_can_insert_missing_node(self) -> None:
        block = (
            "    <message>\n"
            "        <source>A &amp; B</source>\n"
            '        <translation type="unfinished"></translation>\n'
            "    </message>"
        )
        replaced = replace_translation_node(block, "A と B", None)
        self.assertIn("<source>A &amp; B</source>", replaced)
        self.assertIn("<translation>A と B</translation>", replaced)
        self.assertNotIn("unfinished", replaced)
        missing = "    <message>\n        <source>Hello</source>\n    </message>"
        inserted = replace_translation_node(missing, "こんにちは", None)
        self.assertIn("        <translation>こんにちは</translation>\n    </message>", inserted)

    def test_start_creates_bounded_immutable_tasks_without_old_messages(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, session, manifest = self.start_session(Path(directory), FIXTURE, "--max-records", 2)
            self.assertEqual(6, manifest["target_count"])
            self.assertEqual(3, len(manifest["tasks"]))
            sources = []
            for task in manifest["tasks"]:
                records = self.task_records(session, task)
                self.assertLessEqual(len(records), 2)
                sources.extend(record["source"] for record in records)
            self.assertNotIn("Old wording", sources)
            self.assertNotIn("Removed", sources)

            status = self.run_tool("status", session)
            self.assertIn("INCOMPLETE: dispatch the next bounded tasks", status.stdout)
            self.assertIn("Concurrent agent slots are not a total-capacity limit", status.stdout)

            task_path = session / manifest["tasks"][0]["path"]
            task_path.write_text(task_path.read_text(encoding="utf-8") + "\n", encoding="utf-8")
            result = self.run_tool("status", session, expect=1)
            self.assertIn("checksum mismatch", result.stdout)

    def test_large_catalog_scales_by_adding_bounded_tasks(self) -> None:
        messages = "".join(
            "    <message>\n"
            f"        <source>Message {index} with some source text</source>\n"
            "        <translation type=\"unfinished\"></translation>\n"
            "    </message>\n"
            for index in range(2500)
        )
        fixture = (
            '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n'
            '<TS version="2.1" language="ja">\n<context>\n    <name>Large</name>\n'
            f"{messages}</context>\n</TS>\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            _, session, manifest = self.start_session(
                Path(directory),
                fixture,
                "--suggestions",
                0,
            )
            self.assertEqual(9, len(manifest["tasks"]))
            self.assertEqual(2500, manifest["target_count"])
            for index, task in enumerate(manifest["tasks"]):
                records = self.task_records(session, task)
                expected_count = 100 if index == 8 else 300
                self.assertEqual(expected_count, len(records))
                size = (session / task["path"]).stat().st_size
                self.assertLessEqual(size, 192 * 1024)

    def test_partial_checkpoint_can_merge_and_resume(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, session, manifest = self.start_session(Path(directory))
            task = manifest["tasks"][0]
            records = self.task_records(session, task)
            response = self.response_path(session, task)
            self.append_decisions(response, [self.decision_for(record) for record in records[:2]])
            self.run_tool("check", session, response)
            self.run_tool("merge", session, "--write")
            status = self.run_tool("status", session, "--json")
            payload = json.loads(status.stdout)
            self.assertEqual(2, payload["reviewed"])
            self.assertEqual(4, payload["remaining"])
            self.assertEqual(2, payload["tasks"][0]["checkpointed"])
            self.assertEqual(0, payload["tasks"][0]["unmerged"])

            self.append_decisions(response, [self.decision_for(record) for record in records[2:]])
            self.run_tool("check", session, response, "--require-complete-task")
            self.run_tool("merge", session, "--write")
            status = json.loads(self.run_tool("status", session, "--json").stdout)
            self.assertEqual(0, status["remaining"])

    def test_merge_is_idempotent_and_conflicts_require_replace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, session, manifest = self.start_session(Path(directory))
            task = manifest["tasks"][0]
            record = self.task_records(session, task)[0]
            response = self.response_path(session, task)
            decision = self.decision_for(record)
            self.append_decisions(response, [decision])
            self.run_tool("merge", session, "--write")
            repeated = self.run_tool("merge", session, "--write")
            self.assertIn("Merged 0", repeated.stdout)

            lines = response.read_text(encoding="utf-8").splitlines()
            changed = {"id": record["id"], "translation": "別の %1 {0} ${title} %.1f <strong>訳</strong>"}
            response.write_text(lines[0] + "\n" + json.dumps(changed, ensure_ascii=False) + "\n", encoding="utf-8")
            status = json.loads(self.run_tool("status", session, "--json").stdout)
            self.assertEqual(1, status["unmerged"])
            result = self.run_tool("merge", session, response, "--write", expect=1)
            self.assertIn("conflicts with the merged review", result.stdout)
            self.run_tool("merge", session, response, "--replace", "--write")

    def test_malformed_response_does_not_change_journal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, session, manifest = self.start_session(Path(directory))
            response = self.response_path(session, manifest["tasks"][0])
            self.append_decisions(response, [], malformed_tail='{"id":')
            journal = session / "reviews.jsonl"
            original = journal.read_bytes()
            self.run_tool("merge", session, "--write", expect=1)
            self.assertEqual(original, journal.read_bytes())

    def test_salvage_recovers_valid_decisions_from_damaged_response(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, session, manifest = self.start_session(Path(directory))
            task = manifest["tasks"][0]
            records = self.task_records(session, task)
            response = self.response_path(session, task)
            valid = self.decision_for(records[0])
            source_equal = next(record for record in records if record["source"] == "New wording")
            response.write_text(
                "{record_type: metadata, task_id: task-0001}\n"
                + json.dumps(valid, ensure_ascii=False)
                + "\n"
                + json.dumps(
                    {"id": source_equal["id"], "translation": source_equal["source"]},
                    ensure_ascii=False,
                )
                + "\n",
                encoding="utf-8",
            )
            original = response.read_bytes()

            status = self.run_tool("status", session, "--json", expect=1)
            payload = json.loads(status.stdout)
            self.assertEqual("salvage_responses", payload["recommended_action"])
            self.assertEqual([str(response.resolve())], payload["invalid_response_paths"])

            dry_run = self.run_tool("salvage", session, response)
            self.assertIn("Would salvage 2/6 valid decision(s)", dry_run.stdout)
            self.assertIn("normalized source-equal translation", dry_run.stdout)
            self.assertEqual(original, response.read_bytes())

            repaired = self.run_tool("salvage", session, response, "--write")
            self.assertIn("Salvaged 2/6 valid decision(s)", repaired.stdout)
            self.assertIn("skipped 1 invalid line(s)", repaired.stdout)
            self.run_tool("check", session, response)
            self.run_tool("merge", session, "--write")
            payload = json.loads(self.run_tool("status", session, "--json").stdout)
            self.assertEqual(2, payload["reviewed"])
            self.assertEqual(4, payload["remaining"])
            self.assertEqual("dispatch_workers", payload["recommended_action"])

    def test_response_validation_normalizes_source_copy_and_rejects_plural_shape_tags_and_accelerators(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, session, manifest = self.start_session(Path(directory))
            records = [
                record
                for task in manifest["tasks"]
                for record in self.task_records(session, task)
            ]
            by_source = {record["source"]: record for record in records}
            source_equal = by_source["New wording"]
            normalized, warnings = validate_decision(
                source_equal, {"id": source_equal["id"], "translation": "New wording"}
            )
            self.assertTrue(normalized["accept_source"])
            self.assertIn("normalized source-equal translation", warnings[0])
            source_equal_current = dict(source_equal)
            source_equal_current["current_translation"] = source_equal_current["source"]
            normalized, warnings = validate_decision(
                source_equal_current,
                {"id": source_equal["id"], "accept_current": True},
            )
            self.assertTrue(normalized["accept_source"])
            self.assertIn("normalized source-equal accept_current", warnings[0])
            normalized, _ = validate_decision(
                source_equal, {"id": source_equal["id"], "accept_source": True}
            )
            self.assertTrue(normalized["accept_source"])

            plural = by_source["%n file(s)"]
            with self.assertRaisesRegex(ValueError, "exactly 1"):
                validate_decision(plural, {"id": plural["id"], "plural_translations": ["a", "b"]})
            rich = by_source["Hello %1 {0} ${title} %.1f <strong>world</strong>"]
            with self.assertRaisesRegex(ValueError, "rich-text"):
                validate_decision(
                    rich,
                    {
                        "id": rich["id"],
                        "translation": "こんにちは %1 {0} ${title} %.1f 世界",
                    },
                )
            accelerator = by_source["&Open"]
            with self.assertRaisesRegex(ValueError, "accelerator"):
                validate_decision(accelerator, {"id": accelerator["id"], "translation": "開く"})

    def test_complete_session_applies_atomically_and_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog, session, manifest = self.start_session(root, FIXTURE, "--max-records", 2)
            original = catalog.read_bytes()
            self.complete_responses(session, manifest)
            self.run_tool("merge", session, "--write")
            status = self.run_tool("status", session)
            self.assertIn("REVIEW QUEUE COMPLETE", status.stdout)
            self.run_tool("apply", session)
            self.assertEqual(original, catalog.read_bytes())
            self.run_tool("apply", session, "--write")
            updated = catalog.read_text(encoding="utf-8")

            def normalize_translations(text: str) -> str:
                return TRANSLATION_RE.sub("<translation/>", text)

            self.assertEqual(normalize_translations(FIXTURE), normalize_translations(updated))
            self.assertIn('<translation type="vanished">古い文言</translation>', updated)
            self.assertIn('<translation type="obsolete">削除済み</translation>', updated)
            self.assertNotIn('type="unfinished"', updated)
            repeated = self.run_tool("apply", session, "--write")
            self.assertIn("0 translation(s)", repeated.stdout)
            self.assertEqual(updated, catalog.read_text(encoding="utf-8"))
            self.run_validator(catalog, "--require-complete")

    def test_incomplete_or_stale_session_never_changes_catalog(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog, session, manifest = self.start_session(root)
            original = catalog.read_bytes()
            self.run_tool("apply", session, "--write", expect=1)
            self.assertEqual(original, catalog.read_bytes())

            self.complete_responses(session, manifest)
            self.run_tool("merge", session, "--write")
            catalog.write_text(FIXTURE.replace("New wording", "Changed source"), encoding="utf-8")
            changed = catalog.read_bytes()
            result = self.run_tool("apply", session, "--write", expect=1)
            self.assertIn("source identity has changed", result.stdout)
            self.assertEqual(changed, catalog.read_bytes())

    def test_reviewed_only_apply_atomically_harvests_partial_session(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog, session, manifest = self.start_session(root)
            original = catalog.read_bytes()
            task = manifest["tasks"][0]
            record = self.task_records(session, task)[0]
            self.append_decisions(self.response_path(session, task), [self.decision_for(record)])
            self.run_tool("merge", session, "--write")

            dry_run = self.run_tool("apply", session, "--reviewed-only")
            self.assertIn("Would apply 1 translation(s)", dry_run.stdout)
            self.assertIn("5 remain unreviewed", dry_run.stdout)
            self.assertEqual(original, catalog.read_bytes())

            applied = self.run_tool("apply", session, "--reviewed-only", "--write")
            self.assertIn("Applied 1 translation(s)", applied.stdout)
            updated = catalog.read_text(encoding="utf-8")
            self.assertIn(
                "<translation>こんにちは %1 {0} ${title} %.1f "
                "&lt;strong&gt;世界&lt;/strong&gt;</translation>",
                updated,
            )
            self.assertEqual(5, updated.count('type="unfinished"'))

    def test_target_conflict_fails_but_unrelated_translation_edit_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog, session, manifest = self.start_session(root)
            self.complete_responses(session, manifest)
            self.run_tool("merge", session, "--write")
            catalog.write_text(FIXTURE.replace("こんにちは</translation>", "やあ</translation>"), encoding="utf-8")
            self.run_tool("apply", session, "--write")
            self.assertIn("<translation>やあ</translation>", catalog.read_text(encoding="utf-8"))

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog, session, manifest = self.start_session(root)
            self.complete_responses(session, manifest)
            self.run_tool("merge", session, "--write")
            catalog.write_text(FIXTURE.replace("レジスタ", "外部変更"), encoding="utf-8")
            changed = catalog.read_bytes()
            result = self.run_tool("apply", session, "--write", expect=1)
            self.assertIn("changed outside this session", result.stdout)
            self.assertEqual(changed, catalog.read_bytes())

    def test_empty_active_and_missing_translation_can_be_repaired(self) -> None:
        fixture = FIXTURE.replace(
            "</context>",
            "    <message>\n"
            "        <source>Empty active</source>\n"
            "        <translation></translation>\n"
            "    </message>\n"
            "    <message>\n"
            "        <source>Missing active</source>\n"
            "    </message>\n"
            "</context>",
            1,
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog, session, manifest = self.start_session(
                root, fixture, "--include-empty-active"
            )
            found = {}
            for task in manifest["tasks"]:
                response = self.response_path(session, task)
                decisions = []
                for record in self.task_records(session, task):
                    if record["source"] in {"Empty active", "Missing active"}:
                        found[record["source"]] = True
                        decisions.append({"id": record["id"], "translation": "修復済み"})
                    else:
                        decisions.append(self.decision_for(record))
                self.append_decisions(response, decisions)
            self.assertEqual({"Empty active", "Missing active"}, set(found))
            self.run_tool("merge", session, "--write")
            self.run_tool("apply", session, "--write")
            self.assertEqual(2, catalog.read_text(encoding="utf-8").count("<translation>修復済み</translation>"))

    def test_validation_reports_lines_and_supports_strict_required_tags(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            catalog = Path(directory) / "catalog.ts"
            broken = FIXTURE.replace(
                '<translation type="unfinished"></translation>',
                "<translation>プレースホルダーなし</translation>",
                1,
            )
            catalog.write_text(broken, encoding="utf-8")
            result = self.run_validator(catalog, "--placeholders-only", expect=1)
            self.assertIn("[source: alpha.cpp:1]", result.stdout)

            missing_tag = FIXTURE.replace(
                '<translation type="unfinished"></translation>',
                "<translation>Hello %1 {0} ${title} %.1f world</translation>",
                1,
            )
            catalog.write_text(missing_tag, encoding="utf-8")
            self.run_validator(catalog)
            strict = self.run_validator(catalog, "--strict-required-tags", expect=1)
            self.assertIn("missing rich-text tags", strict.stdout)


if __name__ == "__main__":
    unittest.main()
