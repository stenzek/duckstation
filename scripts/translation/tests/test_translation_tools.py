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
sys.path.insert(0, str(SCRIPTS_DIR))

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
    def run_tool(self, script: str, *arguments: object, expect: int = 0) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [sys.executable, str(SCRIPTS_DIR / "translation" / script), *(str(value) for value in arguments)],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(expect, result.returncode, result.stdout)
        return result

    def test_placeholder_recognition_avoids_percent_and_escaped_brace_false_positives(self) -> None:
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
        self.assertTrue(placeholders_match("Saved at {0:%H:%M} on {0:%Y/%m/%d}", "Guardado {0:%d/%m/%Y}"))
        self.assertFalse(
            placeholder_counts_match("Saved at {0:%H:%M} on {0:%Y/%m/%d}", "Guardado {0:%d/%m/%Y}")
        )
        self.assertFalse(placeholder_counts_match("{0} {0} {1}", "{0} {1} {1}"))
        self.assertFalse(placeholders_match("Use %1 and %2", "Usar %1"))
        self.assertTrue(placeholders_are_subset("{} of %n", "%n"))
        self.assertFalse(placeholders_are_subset("{} of %n", "%n %1"))
        self.assertEqual({"qt:%1": 1}, dict(extract_placeholders("%1x")))

    def test_rich_text_ignores_angle_bracket_labels(self) -> None:
        self.assertFalse(extract_rich_tags("<Parent Directory>"))
        self.assertEqual({"strong": 1, "/strong": 1}, dict(extract_rich_tags("<strong>Text</strong>")))
        self.assertFalse(validate_translation("<strong>Text</strong>", "テキスト"))

    def test_rich_text_tag_pairs_are_balanced(self) -> None:
        self.assertFalse(unbalanced_rich_tags("<html><head/><body><br><hr/></body></html>"))
        self.assertFalse(unbalanced_rich_tags("<strong><b>Text</strong></b>"))
        self.assertEqual({"p": (2, 1), "strong": (1, 0)}, unbalanced_rich_tags("<p><p><strong>Text</p>"))

    def test_fingerprint_ignores_translation_changes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.ts"
            second = Path(directory) / "second.ts"
            first.write_text(FIXTURE, encoding="utf-8")
            second.write_text(FIXTURE.replace("レジスタ", "登録"), encoding="utf-8")
            _, first_messages = parse_catalog(first)
            _, second_messages = parse_catalog(second)
            self.assertEqual(catalog_fingerprint(first_messages), catalog_fingerprint(second_messages))

    def test_translation_node_replacement_preserves_message_text(self) -> None:
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

    def test_end_to_end_extract_apply_validate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "catalog.ts"
            batch = root / "batch.jsonl"
            catalog.write_text(FIXTURE, encoding="utf-8")
            original = catalog.read_bytes()

            self.run_tool("extract_ts.py", catalog, "--output", batch, "--suggestions", 3)
            lines = [json.loads(line) for line in batch.read_text(encoding="utf-8").splitlines()]
            records = [line for line in lines if line["record_type"] == "message"]
            self.assertEqual(5, len(records))
            self.assertNotIn("Old wording", {record["source"] for record in records})
            new_wording = next(record for record in records if record["source"] == "New wording")
            self.assertTrue(any(item["type"] == "vanished" for item in new_wording["suggestions"]))

            translations: dict[tuple[str, str], str | list[str]] = {
                (
                    "Alpha",
                    "Hello %1 {0} ${title} %.1f <strong>world</strong>",
                ): "こんにちは %1 {0} ${title} %.1f <strong>世界</strong>",
                ("Alpha", "Register"): "登録",
                ("Alpha", "%n file(s)"): ["%n ファイル"],
                ("Alpha", "New wording"): "新しい文言",
                ("Beta", "Register"): "登録",
            }
            for record in records:
                target = translations[(record["context"], record["source"])]
                if isinstance(target, list):
                    record["target_plural_translations"] = target
                else:
                    record["target_translation"] = target
            batch.write_text(
                "\n".join(json.dumps(line, ensure_ascii=False) for line in lines) + "\n",
                encoding="utf-8",
            )

            self.run_tool("apply_ts.py", catalog, batch)
            self.assertEqual(original, catalog.read_bytes(), "dry-run modified the catalog")
            self.run_tool("apply_ts.py", catalog, batch, "--write")
            updated = catalog.read_text(encoding="utf-8")

            def normalize_translations(text: str) -> str:
                return TRANSLATION_RE.sub("<translation/>", text)

            self.assertEqual(normalize_translations(FIXTURE), normalize_translations(updated))
            self.assertIn('<translation type="vanished">古い文言</translation>', updated)
            self.assertIn('<translation type="obsolete">削除済み</translation>', updated)
            self.assertNotIn('type="unfinished"', updated)
            self.run_tool("validate_ts.py", catalog, "--require-complete")

    def test_accept_current_and_balanced_batches(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "catalog.ts"
            batches = root / "batches"
            catalog.write_text(FIXTURE, encoding="utf-8")
            self.run_tool("extract_ts.py", catalog, "--batch-dir", batches, "--batches", 3)
            files = sorted(batches.glob("*.jsonl"))
            self.assertEqual(3, len(files))
            found = False
            for path in files:
                lines = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
                for line in lines:
                    if line.get("context") == "Alpha" and line.get("source") == "Register":
                        line["accept_current"] = True
                        found = True
                path.write_text(
                    "\n".join(json.dumps(line, ensure_ascii=False) for line in lines) + "\n",
                    encoding="utf-8",
                )
            self.assertTrue(found)
            result = self.run_tool("apply_ts.py", catalog, *files)
            self.assertIn("Would apply 1 translation", result.stdout)

    def test_batches_apply_sequentially_without_invalidating_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "catalog.ts"
            batches = root / "batches"
            catalog.write_text(FIXTURE, encoding="utf-8")
            self.run_tool("extract_ts.py", catalog, "--batch-dir", batches, "--batches", 2)
            files = sorted(batches.glob("*.jsonl"))
            self.assertEqual(2, len(files))
            for path in files:
                lines = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
                record = next(line for line in lines if line["record_type"] == "message")
                if record["numerus"]:
                    record["target_plural_translations"] = [record["source"]]
                else:
                    record["target_translation"] = record["source"]
                path.write_text(
                    "\n".join(json.dumps(line, ensure_ascii=False) for line in lines) + "\n",
                    encoding="utf-8",
                )
            self.run_tool("apply_ts.py", catalog, files[0], "--write")
            self.run_tool("apply_ts.py", catalog, files[1], "--write")

    def test_stale_catalog_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalog = root / "catalog.ts"
            batch = root / "batch.jsonl"
            catalog.write_text(FIXTURE, encoding="utf-8")
            self.run_tool("extract_ts.py", catalog, "--output", batch)
            catalog.write_text(FIXTURE.replace("New wording", "Changed source"), encoding="utf-8")
            result = self.run_tool("apply_ts.py", catalog, batch, expect=1)
            self.assertIn("source identity has changed", result.stdout)

    def test_validation_reports_catalog_and_source_lines(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            catalog = Path(directory) / "catalog.ts"
            broken = FIXTURE.replace(
                '<translation type="unfinished"></translation>',
                "<translation>プレースホルダーなし</translation>",
                1,
            )
            catalog.write_text(broken, encoding="utf-8")
            translation_offset = broken.index("<translation>プレースホルダーなし</translation>")
            expected_line = broken.count("\n", 0, translation_offset) + 1
            result = self.run_tool("validate_ts.py", catalog, "--placeholders-only", expect=1)
            self.assertIn(f"{catalog}:{expected_line}:", result.stdout)
            self.assertIn("[source: alpha.cpp:1]", result.stdout)

    def test_validation_warns_for_placeholder_count_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            catalog = Path(directory) / "catalog.ts"
            catalog.write_text(
                FIXTURE.replace(
                    "Hello %1 {0} ${title} %.1f &lt;strong&gt;world&lt;/strong&gt;",
                    "Use {0}, then use {0} again",
                ).replace(
                    '<translation type="unfinished"></translation>',
                    "<translation>Usar {0}</translation>",
                    1,
                ),
                encoding="utf-8",
            )
            result = self.run_tool("validate_ts.py", catalog, "--placeholders-only")
            self.assertIn("WARNING:", result.stdout)
            self.assertIn("placeholder count mismatch", result.stdout)


if __name__ == "__main__":
    unittest.main()
