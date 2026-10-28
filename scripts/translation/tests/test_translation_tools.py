#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPTS_DIR = Path(__file__).resolve().parents[2]
REPO_ROOT = SCRIPTS_DIR.parent
TRANSLATE = SCRIPTS_DIR / "translation" / "translate_ts.py"
sys.path.insert(0, str(SCRIPTS_DIR))

from translation import lint_ts, review_ts  # noqa: E402
from translation.translate_ts import split_tasks, validate_decision  # noqa: E402
from translation.ts_utils import (  # noqa: E402
    TRANSLATION_RE,
    atomic_write_text,
    catalog_fingerprint,
    extract_placeholders,
    extract_rich_tags,
    jsonl_text,
    parse_catalog,
    placeholder_counts_match,
    placeholders_are_subset,
    placeholders_match,
    replace_translation_node,
    text_sha256,
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
        self.run_tool("start", catalog, "--session-dir", session, "--require-semantic-review", *extra)
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

    def approve_session(self, session: Path, *options: str) -> Path:
        self.run_tool("prepare-review", session, *options)
        audit = Path(json.loads((session / "quality.json").read_text())["session"])
        manifest = json.loads((audit / "manifest.json").read_text())
        for task in manifest["tasks"]:
            decisions = [{"id": record["id"], "outcome": "pass", "reviewer": "fixture reviewer",
                          "source_reason": "Reviewed fixture identifier"}
                         for record in self.task_records(audit, task)]
            self.append_decisions(self.response_path(audit, task), decisions)
        result = subprocess.run([sys.executable, str(SCRIPTS_DIR / "translation/review_ts.py"),
                                 "merge", str(audit), "--write"], capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        return audit

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
        self.assertFalse(placeholders_are_subset("%n files", "%n %n ファイル"))

    def test_rich_text_helpers_ignore_labels_and_find_unbalanced_tags(self) -> None:
        self.assertFalse(extract_rich_tags("<Parent Directory>"))
        self.assertEqual({"strong": 1, "/strong": 1}, dict(extract_rich_tags("<strong>Text</strong>")))
        self.assertFalse(validate_translation("<strong>Text</strong>", "テキスト"))
        self.assertFalse(unbalanced_rich_tags("<html><head/><body><br><hr/></body></html>"))
        self.assertEqual({"p": (2, 1), "strong": (1, 0)}, unbalanced_rich_tags("<p><p><strong>Text</p>"))

    def test_file_dialog_filters_allow_localized_labels_and_pattern_order(self) -> None:
        cases = [
            ("PNG Images (*.png)", "PNG 图像 (*.png)"),
            ("Images (*.jpg *.jpeg);;All Files (*)", "图像（照片） (*.jpeg   *.jpg);;所有文件 (*)"),
            ("Images (PNG) (*.png)", "图像 (PNG) (*.png)"),
            ("Saves (slot?.sav)", "存档 (slot?.sav)"),
            ("Use files (*.png) here.", "在此处使用PNG文件。"),
            ("Scaling (Nearest)", "缩放（邻近）"),
        ]
        for source, translation in cases:
            with self.subTest(source=source, translation=translation):
                self.assertEqual([], validate_translation(source, translation))

    def test_file_dialog_filters_reject_broken_syntax_and_changed_patterns(self) -> None:
        source = "PNG Images (*.png);;JPEG Images (*.jpg *.jpeg)"
        translations = [
            "PNG 图像（*.png）;;JPEG 图像（*.jpg *.jpeg）",
            "PNG 图像 (*.png);JPEG 图像 (*.jpg *.jpeg)",
            "PNG 图像 (*.png)；；JPEG 图像 (*.jpg *.jpeg)",
            "PNG 图像 (*.png);;JPEG 图像 (*.jpg)",
            "PNG 图像 (*.bmp);;JPEG 图像 (*.jpg *.jpeg)",
            "PNG 图像 (*.png);;JPEG 图像 (*.jpg *.jpeg *.gif)",
            "PNG 图像 (*.png);;JPEG 图像 (*.jpg *.jpg)",
            "图像 (*.png *.jpg *.jpeg)",
            "PNG 图像 (*.png);;JPEG 图像 (*.jpg *.jpeg);;所有文件 (*)",
        ]
        for translation in translations:
            with self.subTest(translation=translation):
                self.assertIn("file-dialog filter mismatch", validate_translation(source, translation)[0])
        self.assertIn("file-dialog filter mismatch", validate_translation("All Files (*)", "所有文件 (*.*)")[0])

    def test_file_dialog_filter_validation_reaches_lint_and_session_decisions(self) -> None:
        fixture = FIXTURE.replace("New wording", "PNG Images (*.png)")
        with tempfile.TemporaryDirectory() as directory:
            catalog, session, manifest = self.start_session(Path(directory), fixture)
            record = next(
                record
                for task in manifest["tasks"]
                for record in self.task_records(session, task)
                if record["source"] == "PNG Images (*.png)"
            )
            with self.assertRaisesRegex(ValueError, "file-dialog filter mismatch"):
                validate_decision(record, {"id": record["id"], "translation": "PNG 图像（*.png）"})
            normalized, _ = validate_decision(record, {"id": record["id"], "translation": "PNG 图像 (*.png)"})
            self.assertEqual("PNG 图像 (*.png)", normalized["translation"])

            text = catalog.read_text(encoding="utf-8").replace(
                "<source>PNG Images (*.png)</source>\n        <translation type=\"unfinished\"></translation>",
                "<source>PNG Images (*.png)</source>\n        <translation>PNG 图像（*.png）</translation>",
            )
            catalog.write_text(text, encoding="utf-8")
            result = self.run_validator(catalog, expect=1)
            self.assertIn("file-dialog filter mismatch", result.stdout)
            self.run_validator(catalog, "--placeholders-only")

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
            for other_task in manifest["tasks"][1:]:
                self.append_decisions(self.response_path(session, other_task),
                                      [self.decision_for(record) for record in self.task_records(session, other_task)])
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
            response.write_bytes(response.read_bytes() + b'\xc3')
            utf8_repaired = self.run_tool("salvage", session, response, "--write")
            self.assertIn("Salvaged 2/6", utf8_repaired.stdout)
            self.assertIn("invalid UTF-8", utf8_repaired.stdout)
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
            self.assertIn("TRANSLATION QUEUE COMPLETE", status.stdout)
            blocked = self.run_tool("apply", session, "--write", expect=1)
            self.assertIn("semantic review is required", blocked.stdout)
            self.assertEqual(original, catalog.read_bytes())
            self.approve_session(session)
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
            final_status = json.loads(self.run_tool("status", session, "--json").stdout)
            self.assertTrue(final_status["semantic_review"]["approved"])
            self.assertEqual("final_validation", final_status["recommended_action"])
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

            self.approve_session(session, "--reviewed-only")
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
            self.approve_session(session)
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
            self.approve_session(session)
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

    def test_validation_can_reject_locations_and_old_messages(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            catalog = Path(directory) / "catalog.ts"
            catalog.write_text(FIXTURE, encoding="utf-8")

            locations = self.run_validator(catalog, "--reject-locations", expect=1)
            self.assertIn("source location information is not permitted", locations.stdout)

            obsolete = self.run_validator(catalog, "--reject-obsolete", expect=1)
            self.assertIn("vanished message is not permitted", obsolete.stdout)
            self.assertIn("obsolete message is not permitted", obsolete.stdout)


    def test_partial_finished_plural_is_incomplete_and_selected_for_repair(self) -> None:
        fixture = ('<TS language="es"><context><name>Count</name><message numerus="yes">'
                   '<source>%n file(s)</source><translation><numerusform>%n archivo</numerusform>'
                   '<numerusform></numerusform></translation></message></context></TS>')
        with tempfile.TemporaryDirectory() as directory:
            catalog, session, manifest = self.start_session(Path(directory), fixture, "--include-empty-active")
            self.assertEqual(1, manifest["target_count"])
            result = self.run_validator(catalog, "--require-complete", expect=1)
            self.assertIn("plural form 2 is empty", result.stdout)
            self.run_validator(catalog, "--check-finished", expect=1)

    def test_format_details_nesting_and_protected_attributes_are_checked(self) -> None:
        failures = [
            ("Address {0:08X}", "アドレス {0}"),
            ("Size %zu", "サイズ"),
            ("Value %lld", "値 %d"),
            ("<b><i>Warning</i></b>", "<b><i>警告</b></i>"),
            ('<a href="https://duckstation.org/">Website</a>', '<a href="https://example.org/">サイト</a>'),
        ]
        for source, target in failures:
            with self.subTest(source=source), self.assertRaises(ValueError):
                validate_decision({"id": "probe", "source": source}, {"id": "probe", "translation": target})
        validate_decision({"id": "probe", "source": "{} {:08X}"},
                          {"id": "probe", "translation": "{1:08X} {0}"})
        validate_decision({"id": "probe", "source": "Open file {:{}}"},
                          {"id": "probe", "translation": "Datei {0:{1}} öffnen"})
        validate_decision({"id": "probe", "source": "{:{}} {}"},
                          {"id": "probe", "translation": "{2} {0:{1}}"})
        with self.assertRaises(ValueError):
            validate_decision({"id": "probe", "source": "{:{}} {}"},
                              {"id": "probe", "translation": "{1} {0:{2}}"})
        self.assertEqual({}, extract_placeholders("%%s %%1"))
        self.assertEqual({"printf:%zu": 1, "printf:%lld": 1, "printf:%.*s": 1},
                         dict(extract_placeholders("%zu %lld %.*s")))

    def test_qt_suffixes_and_percent_language_suffixes_are_not_printf_tokens(self) -> None:
        cases = [("%1x Native", "ネイティブの%1倍"), ("%1d ago", "%1 日前"),
                 ("less than 1%", "1%'den az"), ("{:.1f}%", "{:.1f}%'ində")]
        for source, target in cases:
            with self.subTest(source=source):
                validate_decision({"id": "probe", "source": source}, {"id": "probe", "translation": target})

    def test_small_contexts_share_bounded_tasks_and_review_gate_is_explicit(self) -> None:
        records = [{"id": str(index), "context": f"Context{index}"} for index in range(117)]
        self.assertEqual(1, len(split_tasks(records, 300, 192 * 1024)))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "catalog.ts"
            catalog.write_text(FIXTURE)
            session = root / "session"
            self.run_tool("start", catalog, "--session-dir", session)
            manifest = json.loads((session / "manifest.json").read_text())
            self.assertFalse(manifest["require_semantic_review"])
            self.complete_responses(session, manifest)
            self.run_tool("merge", session, "--write")
            self.run_tool("apply", session, "--write")

    def test_needs_context_does_not_count_as_completed_work(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, session, manifest = self.start_session(Path(directory))
            task = manifest["tasks"][0]
            record = self.task_records(session, task)[0]
            self.append_decisions(self.response_path(session, task),
                                  [{"id": record["id"], "needs_context": "Which value does this argument represent?"}])
            self.run_tool("merge", session, "--write")
            status = json.loads(self.run_tool("status", session, "--json").stdout)
            self.assertEqual(6, status["remaining"])
            self.assertEqual(0, status["unmerged"])
            self.assertIn(record["id"], status["tasks"][0]["needs_context"])

    def test_candidate_changes_require_new_semantic_approval(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            catalog, session, manifest = self.start_session(Path(directory))
            self.complete_responses(session, manifest)
            self.run_tool("merge", session, "--write")
            first = self.approve_session(session)
            task = manifest["tasks"][0]
            response = self.response_path(session, task)
            lines = [json.loads(line) for line in response.read_text().splitlines()]
            lines[1]["translation"] = "別の %1 {0} ${title} %.1f <strong>訳</strong>"
            response.write_text("".join(json.dumps(line, ensure_ascii=False) + "\n" for line in lines))
            self.run_tool("merge", session, response, "--replace", "--write")
            original = catalog.read_bytes()
            self.run_tool("apply", session, "--write", expect=1)
            self.assertEqual(original, catalog.read_bytes())
            self.run_tool("prepare-review", session)
            second = Path(json.loads((session / "quality.json").read_text())["session"])
            self.assertNotEqual(first, second)
            journal = [json.loads(line) for line in (second / "reviews.jsonl").read_text().splitlines()]
            self.assertEqual(5, len(journal) - 1)

    def test_language_metadata_and_message_ids_are_part_of_conflict_checks(self) -> None:
        fixture = FIXTURE.replace('<message>', '<message id="test">', 1)
        with tempfile.TemporaryDirectory() as directory:
            catalog, session, _ = self.start_session(Path(directory), fixture)
            catalog.write_text(fixture.replace('language="ja"', 'language="ko"'))
            result = self.run_tool("status", session, expect=1)
            self.assertIn("language/version metadata", result.stdout)
            catalog.write_text(fixture.replace('id="test"', 'id="changed"'))
            result = self.run_tool("status", session, expect=1)
            self.assertIn("source identity", result.stdout)

    def test_baseline_tolerates_only_unchanged_existing_defects(self) -> None:
        fixture = ('<TS language="ja"><context><name>Menu</name><message>'
                   '<source>&amp;Open</source><translation>開く</translation></message></context></TS>')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "catalog.ts"
            baseline = root / "baseline.ts"
            catalog.write_text(fixture)
            baseline.write_text(fixture)
            self.run_validator(catalog, "--strict-accelerators", "--baseline", baseline)
            catalog.write_text(fixture.replace("開く", "開いて"))
            self.run_validator(catalog, "--strict-accelerators", "--baseline", baseline, expect=1)

    def test_strict_plural_validation_allows_redundant_argument_omission(self) -> None:
        fixture = ('<TS language="de"><context><name>Count</name><message numerus="yes">'
                   '<source>Found %n file(s) in %1 (folder: %1).</source><translation>'
                   '<numerusform>%n Datei in %1 gefunden.</numerusform>'
                   '<numerusform>%n Dateien in %1 (Ordner: %1) gefunden.</numerusform>'
                   '</translation></message></context></TS>')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "catalog.ts"
            path.write_text(fixture)
            self.run_validator(path, "--require-complete", "--strict-placeholders")


def catalog(first: str = "開く", second: str = "閉じる") -> str:
    return ('<TS language="ja"><context><name>Menu</name>\n'
            f'<message><source>Open</source><translation>{first}</translation></message>\n'
            f'<message><source>Close</source><translation>{second}</translation></message>\n'
            '</context></TS>')


class ReviewTests(unittest.TestCase):
    def run_review(self, *args: object, expect: int = 0) -> str:
        result = subprocess.run([sys.executable, "-B", str(Path(review_ts.__file__)), *map(str, args)],
                                text=True, capture_output=True)
        self.assertEqual(expect, result.returncode, result.stdout + result.stderr)
        return result.stdout

    def test_partial_audit_resume_salvage_and_exact_approval(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "catalog.ts"
            path.write_text(catalog())
            session = review_ts.start_session(path, root / "audit", max_records=1)
            _, manifest, records, tasks = review_ts.load_session(session)
            task = next(iter(tasks.values()))
            response = session / task["response_path"]
            decision = {"id": records[0]["id"], "outcome": "pass", "reviewer": "reviewer 1"}
            response.write_text(jsonl_text(review_ts.metadata(manifest, task), [decision]) + '{"id":')
            original_journal = (session / "reviews.jsonl").read_bytes()
            self.run_review("merge", session, response, "--write", expect=1)
            self.assertEqual(original_journal, (session / "reviews.jsonl").read_bytes())
            self.run_review("salvage", session, response, "--write")
            response.write_bytes(response.read_bytes() + b'\xc3')
            self.run_review("salvage", session, response, "--write")
            self.run_review("merge", session, response, "--write")
            payload = json.loads(self.run_review("status", session, "--json"))
            self.assertEqual((1, 1), (payload["reviewed"], payload["remaining"]))
            review_ts.require_approved(session, manifest["catalog_attributes"], records[:1])
            with self.assertRaises(ValueError):
                review_ts.require_approved(session, manifest["catalog_attributes"], records)
            changed = {**records[0], "current_translation": "別の訳"}
            with self.assertRaises(ValueError):
                review_ts.require_approved(session, manifest["catalog_attributes"], [changed])
            path.write_text(catalog(second="閉じて"))
            self.run_review("status", session, expect=1)
            second = review_ts.start_session(path, root / "audit-2", previous=session)
            _, next_manifest, next_records, _ = review_ts.load_session(second)
            retained = review_ts.load_journal(second, next_manifest, next_records)
            self.assertEqual({records[0]["id"]}, set(retained))

    def test_source_equal_requires_a_reviewed_reason_and_findings_are_validated(self):
        record = {"id": "one", "source": "Vulkan", "current_translation": "Vulkan", "numerus": False}
        result = {"id": "one", "outcome": "pass", "reviewer": "reviewer 2"}
        with self.assertRaisesRegex(ValueError, "source_reason"):
            review_ts.validate_result(record, result)
        review_ts.validate_result(record, {**result, "source_reason": "Established API name"})
        problem = {"id": "one", "outcome": "finding", "reviewer": "reviewer 2", "findings": [
            {"severity": "medium", "category": "accuracy", "reason": "Incorrect argument", "translation": "値"}]}
        record = {**record, "source": "Value %1"}
        with self.assertRaisesRegex(ValueError, "placeholder"):
            review_ts.validate_result(record, problem)

    def test_review_response_ownership_snapshot_and_locale_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "catalog.ts"
            path.write_text(catalog())
            session = review_ts.start_session(path, root / "audit", max_records=1)
            _, manifest, records, tasks = review_ts.load_session(session)
            task = next(iter(tasks.values()))
            response = session / task["response_path"]
            response.write_text(jsonl_text(review_ts.metadata(manifest, task), [
                {"id": records[1]["id"], "outcome": "pass", "reviewer": "reviewer 3"}]))
            self.run_review("check", session, response, expect=1)
            with self.assertRaisesRegex(ValueError, "language/version"):
                review_ts.require_approved(session, {"language": "ko"}, records)
            (session / "snapshot.ts").write_text(catalog(first="changed"))
            with self.assertRaisesRegex(ValueError, "checksum"):
                review_ts.load_session(session)

    def test_atomic_update_preserves_a_concurrent_edit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "catalog.ts"
            path.write_text("original")
            with patch("translation.ts_utils.os.fsync", side_effect=lambda _: path.write_text("external edit")):
                with self.assertRaisesRegex(ValueError, "changed during"):
                    atomic_write_text(path, "replacement", expected_sha256=text_sha256("original"))
            self.assertEqual("external edit", path.read_text())

if __name__ == "__main__":
    unittest.main()
