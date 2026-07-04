#!/usr/bin/env python3
"""Shared Qt TS catalog parsing, identity, and validation helpers."""

from __future__ import annotations

import bisect
import collections
import hashlib
import json
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Sequence


SKIPPED_TYPES = frozenset({"vanished", "obsolete"})
SCHEMA = "duckstation-qt-translation-batch-v1"

QT_PLACEHOLDER_RE = re.compile(r"%(?:L?\d+|Ln|n)")
PRINTF_PLACEHOLDER_RE = re.compile(r"%(?!%)(?:(?:[-+0#]+\d*|\d+)?(?:\.\d+)?[diuoxXfFeEgGaAcsp])")
FMT_PLACEHOLDER_RE = re.compile(r"(?<!\{)\{(?:\d+)?(?::[^{}]+)?\}(?!\})")
TEMPLATE_PLACEHOLDER_RE = re.compile(r"\$\{[^{}]+\}")
RICH_TAG_RE = re.compile(
    r"<\s*(/?)\s*(html|head|body|p|span|strong|b|i|u|br|hr|h[1-6]|a|table|tr|td|ul|ol|li)\b[^>]*>",
    re.IGNORECASE,
)

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

    def as_payload(self) -> dict[str, object]:
        return {
            "context": self.context,
            "source": self.source,
            "comment": self.comment,
            "extracomment": self.extracomment,
            "numerus": self.numerus,
            "occurrence": self.occurrence,
        }

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


def element_text(element: ET.Element | None) -> str:
    return "" if element is None else "".join(element.itertext())


def identity_base(context: str, message: ET.Element) -> tuple[str, str, str, str, bool]:
    return (
        context,
        message.findtext("source") or "",
        message.findtext("comment") or "",
        message.findtext("extracomment") or "",
        message.get("numerus") == "yes",
    )


def iter_catalog_messages(root: ET.Element) -> Iterator[CatalogMessage]:
    for context_element in root.findall("context"):
        context = context_element.findtext("name") or ""
        occurrences: collections.Counter[tuple[str, str, str, str, bool]] = collections.Counter()
        for message in context_element.findall("message"):
            base = identity_base(context, message)
            occurrence = occurrences[base]
            occurrences[base] += 1
            identity = MessageIdentity(*base, occurrence)
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
            )


def parse_catalog(path: Path | str) -> tuple[ET.ElementTree, list[CatalogMessage]]:
    tree = ET.parse(path)
    messages = list(iter_catalog_messages(tree.getroot()))
    with Path(path).open("r", encoding="utf-8", newline="") as stream:
        spans = scan_raw_messages(stream.read())
    for message in messages:
        span = spans.get(message.identifier)
        if span is not None:
            message.catalog_line = span.catalog_line
    return tree, messages


def catalog_fingerprint(messages: Iterable[CatalogMessage]) -> str:
    identities = [message.identity.as_payload() for message in messages]
    encoded = json.dumps(identities, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def extract_placeholders(text: str) -> collections.Counter[str]:
    tokens: list[str] = []
    occupied: list[tuple[int, int]] = []
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
                placeholder = f"{placeholder.split(':', 1)[0]}}}"
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
    if placeholders_match(source, translation):
        return True
    source_tokens = extract_placeholders(source)
    translated_tokens = extract_placeholders(translation)
    if set(translated_tokens) <= set(source_tokens):
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
    if not set(translated_non_fmt) <= set(source_non_fmt) or set(source_fmt) != {"{}"}:
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
        if tag in {"br", "hr"} or (not match.group(1) and match.group(0).rstrip().endswith("/>")):
            continue
        (closing if match.group(1) else opening)[tag] += 1

    return {
        tag: (opening[tag], closing[tag])
        for tag in sorted(opening.keys() | closing.keys())
        if opening[tag] != closing[tag]
    }


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
    return problems


def extra_rich_tags(source: str, translation: str) -> collections.Counter[str]:
    return extract_rich_tags(translation) - extract_rich_tags(source)


def missing_rich_tags(source: str, translation: str) -> collections.Counter[str]:
    return extract_rich_tags(source) - extract_rich_tags(translation)


def message_to_batch_record(message: CatalogMessage) -> dict[str, object]:
    return {
        "record_type": "message",
        "id": message.identifier,
        **message.identity.as_payload(),
        "locations": message.locations,
        "current_type": message.translation_type,
        "current_translation": message.translation,
        "current_plural_translations": message.plural_translations,
        "accept_current": False,
        "target_translation": None,
        "target_plural_translations": None,
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


def write_jsonl(path: Path, metadata: dict[str, object], records: Sequence[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(metadata, ensure_ascii=False, sort_keys=True) + "\n")
        for record in records:
            stream.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")


def scan_raw_messages(text: str) -> dict[str, RawMessageSpan]:
    spans: dict[str, RawMessageSpan] = {}
    newline_offsets = [index for index, character in enumerate(text) if character == "\n"]
    for context_match in CONTEXT_RE.finditer(text):
        context_block = context_match.group(0)
        context_name_match = re.search(r"<name>(.*?)</name>", context_block, re.DOTALL)
        if context_name_match is None:
            continue
        context_name = element_text(ET.fromstring(f"<name>{context_name_match.group(1)}</name>"))
        occurrences: collections.Counter[tuple[str, str, str, str, bool]] = collections.Counter()
        for message_match in MESSAGE_RE.finditer(context_block):
            block = message_match.group(0)
            message_element = ET.fromstring(block)
            base = identity_base(context_name, message_element)
            occurrence = occurrences[base]
            occurrences[base] += 1
            identity = MessageIdentity(*base, occurrence)
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
        raise ValueError("message has no translation element")
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
    return block[: match.start()] + replacement + block[match.end() :]
