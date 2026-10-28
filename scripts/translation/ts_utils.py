#!/usr/bin/env python3
"""Shared Qt TS catalog parsing, identity, and validation helpers."""

from __future__ import annotations

import bisect
import collections
import hashlib
import json
import os
import re
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path
from typing import Iterable, Iterator, Sequence


SKIPPED_TYPES = frozenset({"vanished", "obsolete"})

QT_PLACEHOLDER_RE = re.compile(r"%(?:L?\d+|Ln|n)")
PRINTF_PLACEHOLDER_RE = re.compile(
    r"%(?!%)(?:\d+\$)?[-+0#]*(?:\d+|\*(?:\d+\$)?)?"
    r"(?:\.(?:\d+|\*(?:\d+\$)?))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcsp]"
)
FMT_PLACEHOLDER_RE = re.compile(r"(?<!\{)\{(?:\d+)?(?::(?:[^{}]|\{\d*\})*)?\}(?!\})")
TEMPLATE_PLACEHOLDER_RE = re.compile(r"\$\{[^{}]+\}")
RICH_TAG_RE = re.compile(
    r"<\s*(/?)\s*(html|head|body|p|div|span|strong|b|i|em|u|s|sub|sup|pre|code|font|br|hr|img|h[1-6]|a|table|tr|td|th|ul|ol|li)\b[^>]*>",
    re.IGNORECASE,
)
FILE_DIALOG_FILTER_RE = re.compile(r"[^\r\n]*\(([^()]*)\)\s*")

CONTEXT_RE = re.compile(r"<context>.*?</context>", re.DOTALL)
MESSAGE_RE = re.compile(r"<message(?:\s[^>]*)?>.*?</message>", re.DOTALL)
TRANSLATION_RE = re.compile(
    r"<translation(?P<attrs>[^>]*)>.*?</translation>|<translation(?P<self_attrs>[^>]*)/>", re.DOTALL
)


@dataclass(frozen=True)
class MessageIdentity:
    context: str
    source: str
    comment: str
    extracomment: str
    numerus: bool
    occurrence: int
    message_id: str = ""

    def as_payload(self) -> dict[str, object]:
        payload = {
            "context": self.context,
            "source": self.source,
            "comment": self.comment,
            "extracomment": self.extracomment,
            "numerus": self.numerus,
            "occurrence": self.occurrence,
        }
        if self.message_id:
            payload["message_id"] = self.message_id
        return payload

    @property
    def identifier(self) -> str:
        encoded = json.dumps(self.as_payload(), ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        return hashlib.sha256(encoded.encode("utf-8")).hexdigest()[:20]


@dataclass
class CatalogMessage:
    identity: MessageIdentity
    translation_type: str
    translation: str | None
    plural_translations: list[str]
    locations: list[dict[str, str]]
    catalog_line: int | None = None
    message_line: int | None = None
    translator_comment: str = ""

    @property
    def identifier(self) -> str:
        return self.identity.identifier


@dataclass(frozen=True)
class RawMessageSpan:
    identifier: str
    start: int
    end: int
    block: str
    translation_type: str
    numerus: bool
    catalog_line: int
    message_line: int


def element_text(element: ET.Element | None) -> str:
    return "" if element is None else "".join(element.itertext())


def identity_base(context: str, message: ET.Element) -> tuple[str, str, str, str, bool, str]:
    return (
        context,
        message.findtext("source") or "",
        message.findtext("comment") or "",
        message.findtext("extracomment") or "",
        message.get("numerus") == "yes",
        message.get("id", ""),
    )


def iter_catalog_messages(root: ET.Element) -> Iterator[CatalogMessage]:
    for context_element in root.findall("context"):
        context = context_element.findtext("name") or ""
        occurrences: collections.Counter[tuple] = collections.Counter()
        for message in context_element.findall("message"):
            base = identity_base(context, message)
            occurrence = occurrences[base]
            occurrences[base] += 1
            identity = MessageIdentity(*base[:5], occurrence, message_id=base[5])
            translation_element = message.find("translation")
            translation_type = "missing" if translation_element is None else translation_element.get("type", "finished")
            plural_elements = [] if translation_element is None else translation_element.findall("numerusform")
            plural_translations = [element_text(form) for form in plural_elements]
            translation = None if translation_element is None or plural_elements else element_text(translation_element)
            locations = [dict(location.attrib) for location in message.findall("location")]
            yield CatalogMessage(
                identity=identity,
                translation_type=translation_type,
                translation=translation,
                plural_translations=plural_translations,
                locations=locations,
                translator_comment=message.findtext("translatorcomment") or "",
            )


def parse_catalog(path: Path | str) -> tuple[ET.ElementTree, list[CatalogMessage]]:
    with Path(path).open("r", encoding="utf-8", newline="") as stream:
        return parse_catalog_text(stream.read())


def parse_catalog_text(text: str) -> tuple[ET.ElementTree, list[CatalogMessage]]:
    tree = ET.ElementTree(ET.fromstring(text))
    if tree.getroot().tag != "TS":
        raise ValueError("Expected a Qt TS catalog")
    messages = list(iter_catalog_messages(tree.getroot()))
    spans = scan_raw_messages(text)
    for message in messages:
        span = spans.get(message.identifier)
        if span is not None:
            message.catalog_line = span.catalog_line
            message.message_line = span.message_line
    return tree, messages


def catalog_fingerprint(messages: Iterable[CatalogMessage]) -> str:
    identities = [message.identity.as_payload() for message in messages]
    encoded = json.dumps(identities, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def extract_placeholders(text: str) -> collections.Counter[str]:
    tokens: list[str] = []
    occupied: list[tuple[int, int]] = [match.span() for match in re.finditer(r"%%", text)]
    for kind, regex in (
        ("template", TEMPLATE_PLACEHOLDER_RE),
        ("fmt", FMT_PLACEHOLDER_RE),
        ("qt", QT_PLACEHOLDER_RE),
        ("printf", PRINTF_PLACEHOLDER_RE),
    ):
        for match in regex.finditer(text):
            if any(match.start() < end and match.end() > start for start, end in occupied):
                continue
            occupied.append(match.span())
            placeholder = match.group(0)
            if kind == "fmt" and ":" in placeholder:
                field, specification = placeholder.split(":", 1)
                tokens.extend(f"fmt:{nested.group()}" for nested in re.finditer(r"\{\d*\}", specification))
                placeholder = f"{field}}}"
            tokens.append(f"{kind}:{placeholder}")
    return collections.Counter(tokens)


def placeholders_match(source: str, translation: str) -> bool:
    source_tokens = extract_placeholders(source)
    translated_tokens = extract_placeholders(translation)
    if source_tokens == translated_tokens:
        return True

    source_fmt = collections.Counter(
        token.removeprefix("fmt:") for token in source_tokens.elements() if token.startswith("fmt:")
    )
    translated_fmt = collections.Counter(
        token.removeprefix("fmt:") for token in translated_tokens.elements() if token.startswith("fmt:")
    )
    source_non_fmt = collections.Counter(
        token for token in source_tokens.elements() if not token.startswith("fmt:")
    )
    translated_non_fmt = collections.Counter(
        token for token in translated_tokens.elements() if not token.startswith("fmt:")
    )
    if set(source_non_fmt) != set(translated_non_fmt):
        return False

    if set(source_fmt) == set(translated_fmt):
        return True

    # Qt's fmt usage permits translators to number anonymous plain placeholders
    # when reordering arguments, e.g. "{} {}" -> "{1} {0}".
    if set(source_fmt) == {"{}"}:
        expected = collections.Counter(f"{{{index}}}" for index in range(source_fmt["{}"]))
        return translated_fmt == expected
    return False


def placeholder_counts_match(source: str, translation: str) -> bool:
    """Return whether compatible placeholders occur the same number of times."""
    if not placeholders_match(source, translation):
        return False

    source_tokens = extract_placeholders(source)
    translated_tokens = extract_placeholders(translation)
    if source_tokens == translated_tokens:
        return True

    source_fmt = collections.Counter(
        token.removeprefix("fmt:") for token in source_tokens.elements() if token.startswith("fmt:")
    )
    translated_fmt = collections.Counter(
        token.removeprefix("fmt:") for token in translated_tokens.elements() if token.startswith("fmt:")
    )
    source_non_fmt = collections.Counter(
        token for token in source_tokens.elements() if not token.startswith("fmt:")
    )
    translated_non_fmt = collections.Counter(
        token for token in translated_tokens.elements() if not token.startswith("fmt:")
    )
    if set(source_fmt) == {"{}"}:
        fmt_counts_match = sum(source_fmt.values()) == sum(translated_fmt.values())
    else:
        fmt_counts_match = source_fmt == translated_fmt
    return source_non_fmt == translated_non_fmt and fmt_counts_match


def placeholders_are_subset(source: str, translation: str) -> bool:
    """Allow a plural form to omit redundant arguments without adding any."""
    if placeholder_counts_match(source, translation):
        return True
    source_tokens = extract_placeholders(source)
    translated_tokens = extract_placeholders(translation)
    if translated_tokens <= source_tokens:
        return True

    source_fmt = collections.Counter(
        token.removeprefix("fmt:") for token in source_tokens.elements() if token.startswith("fmt:")
    )
    translated_fmt = collections.Counter(
        token.removeprefix("fmt:") for token in translated_tokens.elements() if token.startswith("fmt:")
    )
    source_non_fmt = collections.Counter(
        token for token in source_tokens.elements() if not token.startswith("fmt:")
    )
    translated_non_fmt = collections.Counter(
        token for token in translated_tokens.elements() if not token.startswith("fmt:")
    )
    if not translated_non_fmt <= source_non_fmt or set(source_fmt) != {"{}"}:
        return False
    allowed_numbered = {f"{{{index}}}" for index in range(source_fmt["{}"])}
    return set(translated_fmt) <= allowed_numbered and all(count == 1 for count in translated_fmt.values())


def extract_rich_tags(text: str) -> collections.Counter[str]:
    tags: list[str] = []
    for match in RICH_TAG_RE.finditer(text):
        closing = "/" if match.group(1) else ""
        tags.append(f"{closing}{match.group(2).lower()}")
    return collections.Counter(tags)


def unbalanced_rich_tags(text: str) -> dict[str, tuple[int, int]]:
    """Return paired rich-text elements with unequal opening and closing counts."""
    opening: collections.Counter[str] = collections.Counter()
    closing: collections.Counter[str] = collections.Counter()
    for match in RICH_TAG_RE.finditer(text):
        tag = match.group(2).lower()
        if tag in {"br", "hr", "img"} or (not match.group(1) and match.group(0).rstrip().endswith("/>")):
            continue
        (closing if match.group(1) else opening)[tag] += 1

    return {
        tag: (opening[tag], closing[tag])
        for tag in sorted(opening.keys() | closing.keys())
        if opening[tag] != closing[tag]
    }


class RichTextParser(HTMLParser):
    """Check recognized Qt rich text without treating literal angle labels as tags."""

    def __init__(self, text: str):
        super().__init__(convert_charrefs=True)
        self.stack: list[str] = []
        self.problems: list[str] = []
        self.protected: collections.Counter[tuple[str, str, str]] = collections.Counter()
        self.feed(text)
        self.close()
        if self.stack:
            self.problems.append(f"unclosed rich-text tags: {self.stack}")

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if RICH_TAG_RE.fullmatch(self.get_starttag_text() or "") is None:
            return
        for name, value in attrs:
            if name in {"href", "src"}:
                self.protected[(tag, name, value or "")] += 1
        if tag not in {"br", "hr", "img"}:
            self.stack.append(tag)

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self.handle_starttag(tag, attrs)
        if self.stack and self.stack[-1] == tag:
            self.stack.pop()

    def handle_endtag(self, tag: str) -> None:
        if RICH_TAG_RE.fullmatch(f"</{tag}>") is None or tag in {"br", "hr", "img"}:
            return
        if not self.stack or self.stack[-1] != tag:
            self.problems.append(f"incorrect rich-text nesting at </{tag}>")
        if tag in self.stack:
            del self.stack[len(self.stack) - 1 - self.stack[::-1].index(tag):]


def rich_text_problems(source: str, translation: str) -> list[str]:
    original, translated = RichTextParser(source), RichTextParser(translation)
    problems = list(translated.problems)
    if original.protected != translated.protected:
        problems.append("rich-text link/resource attributes differ from the source")
    return problems


def fmt_arguments(text: str) -> collections.Counter[tuple[int, str]]:
    """Keep argument identities and format specifications, allowing positional reordering."""
    result: collections.Counter[tuple[int, str]] = collections.Counter()
    automatic = 0
    template_spans = [match.span() for match in TEMPLATE_PLACEHOLDER_RE.finditer(text)]
    for match in FMT_PLACEHOLDER_RE.finditer(text):
        if any(match.start() < end and match.end() > start for start, end in template_spans):
            continue
        field, separator, specification = match.group()[1:-1].partition(":")
        index = int(field) if field else automatic
        if not field:
            automatic += 1

        def normalize_nested(match: re.Match[str]) -> str:
            nonlocal automatic
            nested_field = match.group()[1:-1]
            nested_index = int(nested_field) if nested_field else automatic
            if not nested_field:
                automatic += 1
            result[(nested_index, "")] += 1
            return f"{{{nested_index}}}"

        specification = re.sub(r"\{\d*\}", normalize_nested, specification)
        result[(index, specification if separator else "")] += 1
    return result


def accelerator_count(text: str) -> int:
    # HTML attributes and entities are not menu accelerators.
    text = RICH_TAG_RE.sub("", text)
    text = re.sub(r"&(?:#\d+|#x[0-9a-fA-F]+|[A-Za-z]+);", "", text)
    return len(re.findall(r"(?<!&)&(?!&|\s|$)", text.replace("&&", "")))


def file_dialog_filter_patterns(text: str) -> list[collections.Counter[str]] | None:
    """Recognize labeled Qt name filters, ignoring their translatable descriptions."""
    filters: list[collections.Counter[str]] = []
    for part in text.split(";;"):
        match = FILE_DIALOG_FILTER_RE.fullmatch(part.strip())
        if match is None:
            return None
        patterns = match.group(1).split()
        if not patterns or any(not any(wildcard in pattern for wildcard in "*?") for pattern in patterns):
            return None
        filters.append(collections.Counter(patterns))
    return filters


def validate_translation(source: str, translation: str, allow_missing_placeholders: bool = False) -> list[str]:
    problems: list[str] = []
    source_placeholders = extract_placeholders(source)
    translated_placeholders = extract_placeholders(translation)
    placeholders_valid = (
        placeholders_are_subset(source, translation)
        if allow_missing_placeholders
        else placeholders_match(source, translation)
    )
    if not placeholders_valid:
        problems.append(
            f"placeholder mismatch: source={dict(source_placeholders)} translation={dict(translated_placeholders)}"
        )
    original_formats, translated_formats = fmt_arguments(source), fmt_arguments(translation)
    if not set(translated_formats) <= set(original_formats) or (
        not allow_missing_placeholders and set(original_formats) != set(translated_formats)
    ):
        problems.append("fmt argument format specifications differ from the source")
    source_filters = file_dialog_filter_patterns(source)
    if source_filters is not None and file_dialog_filter_patterns(translation) != source_filters:
        problems.append(
            "file-dialog filter mismatch: preserve ASCII parentheses, ';;' separators, "
            "and wildcard patterns in each filter"
        )
    return problems


def extra_rich_tags(source: str, translation: str) -> collections.Counter[str]:
    return extract_rich_tags(translation) - extract_rich_tags(source)


def missing_rich_tags(source: str, translation: str) -> collections.Counter[str]:
    return extract_rich_tags(source) - extract_rich_tags(translation)


def message_to_task_record(message: CatalogMessage) -> dict[str, object]:
    return {
        "record_type": "message",
        "id": message.identifier,
        **message.identity.as_payload(),
        "locations": message.locations,
        "current_type": message.translation_type,
        "current_translation": message.translation,
        "current_plural_translations": message.plural_translations,
        "plural_arity": len(message.plural_translations),
        "catalog_line": message.catalog_line,
        "message_line": message.message_line,
        "translator_comment": message.translator_comment,
        "suggestions": [],
    }


def load_jsonl(paths: Sequence[Path | str]) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    metadata: list[dict[str, object]] = []
    records: list[dict[str, object]] = []
    for path_value in paths:
        path = Path(path_value)
        with path.open("r", encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    item = json.loads(line)
                except json.JSONDecodeError as error:
                    raise ValueError(f"{path}:{line_number}: invalid JSON: {error}") from error
                if item.get("record_type") == "metadata":
                    metadata.append(item)
                elif item.get("record_type") == "message":
                    records.append(item)
                else:
                    raise ValueError(f"{path}:{line_number}: unknown record_type")
    return metadata, records


def atomic_write_text(
    path: Path, text: str, mode: int | None = None, expected_sha256: str | None = None
) -> None:
    """Write text beside its destination and atomically replace the destination."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if mode is None and path.exists():
        mode = path.stat().st_mode
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", newline="", dir=path.parent, delete=False
        ) as stream:
            temporary_name = stream.name
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        if mode is not None:
            os.chmod(temporary_name, mode)
        if expected_sha256 is not None and hashlib.sha256(path.read_bytes()).hexdigest() != expected_sha256:
            raise ValueError(f"{path}: file changed during atomic update")
        os.replace(temporary_name, path)
    finally:
        if temporary_name and os.path.exists(temporary_name):
            os.unlink(temporary_name)


def jsonl_text(metadata: dict[str, object], records: Sequence[dict[str, object]]) -> str:
    lines = [json.dumps(metadata, ensure_ascii=False, sort_keys=True)]
    lines.extend(json.dumps(record, ensure_ascii=False, sort_keys=True) for record in records)
    return "\n".join(lines) + "\n"


def text_sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def scan_raw_messages(text: str) -> dict[str, RawMessageSpan]:
    spans: dict[str, RawMessageSpan] = {}
    newline_offsets = [index for index, character in enumerate(text) if character == "\n"]
    for context_match in CONTEXT_RE.finditer(text):
        context_block = context_match.group(0)
        context_name_match = re.search(r"<name>(.*?)</name>", context_block, re.DOTALL)
        if context_name_match is None:
            continue
        context_name = element_text(ET.fromstring(f"<name>{context_name_match.group(1)}</name>"))
        occurrences: collections.Counter[tuple] = collections.Counter()
        for message_match in MESSAGE_RE.finditer(context_block):
            block = message_match.group(0)
            message_element = ET.fromstring(block)
            base = identity_base(context_name, message_element)
            occurrence = occurrences[base]
            occurrences[base] += 1
            identity = MessageIdentity(*base[:5], occurrence, message_id=base[5])
            translation = message_element.find("translation")
            translation_type = "missing" if translation is None else translation.get("type", "finished")
            absolute_start = context_match.start() + message_match.start()
            absolute_end = context_match.start() + message_match.end()
            translation_match = TRANSLATION_RE.search(block)
            diagnostic_offset = absolute_start + (translation_match.start() if translation_match else 0)
            span = RawMessageSpan(
                identifier=identity.identifier,
                start=absolute_start,
                end=absolute_end,
                block=block,
                translation_type=translation_type,
                numerus=message_element.get("numerus") == "yes",
                catalog_line=bisect.bisect_left(newline_offsets, diagnostic_offset) + 1,
                message_line=bisect.bisect_left(newline_offsets, absolute_start) + 1,
            )
            if span.identifier in spans:
                raise ValueError(f"duplicate stable message id: {span.identifier}")
            spans[span.identifier] = span
    return spans


def xml_escape_text(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def remove_unfinished_attribute(attributes: str) -> str:
    attributes = re.sub(r"\s+type=(?:\"unfinished\"|'unfinished')", "", attributes)
    return attributes.rstrip()


def replace_translation_node(block: str, singular: str | None, plurals: Sequence[str] | None) -> str:
    match = TRANSLATION_RE.search(block)
    if match is None:
        closing = block.rfind("</message>")
        if closing < 0:
            raise ValueError("message has no closing element")
        line_start = block.rfind("\n", 0, closing) + 1
        message_indent = block[line_start:closing]
        indent = f"{message_indent}    "
        attributes = ""
    else:
        attributes = remove_unfinished_attribute(match.group("attrs") or match.group("self_attrs") or "")
        line_start = block.rfind("\n", 0, match.start()) + 1
        indent = block[line_start:match.start()]
    if plurals is not None:
        forms = "\n".join(f"{indent}    <numerusform>{xml_escape_text(value)}</numerusform>" for value in plurals)
        replacement = f"<translation{attributes}>\n{forms}\n{indent}</translation>"
    else:
        if singular is None:
            raise ValueError("missing singular target translation")
        replacement = f"<translation{attributes}>{xml_escape_text(singular)}</translation>"
    if match is None:
        return f"{block[:line_start]}{indent}{replacement}\n{block[line_start:]}"
    return block[: match.start()] + replacement + block[match.end() :]
