
#!/usr/bin/env python3
"""
Qt Translation Placeholder Verifier

This script verifies that placeholders in Qt translation files are consistent
between source and translation strings.

Placeholder rules:
- {} placeholders: translation must have same total count as source
- {n} placeholders: translation must use same numbers as source (can repeat)
"""

import xml.etree.ElementTree as ET
import re
import sys
from typing import List, Set, Tuple, Dict
from pathlib import Path


def extract_placeholders(text: str) -> Tuple[int, Set[int]]:
    """
    Extract placeholder information from a string.
    
    Returns:
        Tuple of (unnamed_count, set_of_numbered_placeholders)
    """
    # Find all placeholders
    placeholders = re.findall(r'\{(\d*)\}', text)
    
    unnamed_count = 0
    numbered_set = set()
    
    for placeholder in placeholders:
        if placeholder == '':
            unnamed_count += 1
        else:
            numbered_set.add(int(placeholder))
    
    return unnamed_count, numbered_set


def verify_placeholders(source: str, translation: str) -> Tuple[bool, str]:
    """
    Verify that placeholders in source and translation are consistent.
    
    Returns:
        Tuple of (is_valid, error_message)
    """
    if not translation.strip():
        return True, ""  # Empty translations are valid
    
    source_unnamed, source_numbered = extract_placeholders(source)
    trans_unnamed, trans_numbered = extract_placeholders(translation)
    
    # Check if mixing numbered and unnumbered placeholders
    if source_unnamed > 0 and len(source_numbered) > 0:
        return False, "Source mixes numbered and unnumbered placeholders"
    
    if trans_unnamed > 0 and len(trans_numbered) > 0:
        return False, "Translation mixes numbered and unnumbered placeholders"
    
    # If source uses unnumbered placeholders
    if source_unnamed > 0:
        # Translation can use either unnumbered or numbered placeholders
        if trans_unnamed > 0:
            # Both use unnumbered - simple count match
            if source_unnamed != trans_unnamed:
                return False, f"Placeholder count mismatch: source has {source_unnamed}, translation has {trans_unnamed}"
        elif len(trans_numbered) > 0:
            # Source uses unnumbered, translation uses numbered
            # Check that numbered placeholders are 0-based and consecutive up to source count
            expected_numbers = set(range(source_unnamed))
            if trans_numbered != expected_numbers:
                if max(trans_numbered) >= source_unnamed:
                    return False, f"Numbered placeholders exceed source count: translation uses {{{max(trans_numbered)}}}, but source only has {source_unnamed} placeholders"
                elif min(trans_numbered) != 0:
                    return False, f"Numbered placeholders must start from 0: found {{{min(trans_numbered)}}}"
                else:
                    missing = expected_numbers - trans_numbered
                    return False, f"Missing numbered placeholders: {{{','.join(map(str, sorted(missing)))}}}"
    
    # If source uses numbered placeholders
    elif len(source_numbered) > 0:
        if trans_unnamed > 0:
            return False, "Source uses numbered {n} but translation uses unnumbered {}"
        if source_numbered != trans_numbered:
            missing = source_numbered - trans_numbered
            extra = trans_numbered - source_numbered
            error_parts = []
            if missing:
                error_parts.append(f"missing {{{','.join(map(str, sorted(missing)))}}}")
            if extra:
                error_parts.append(f"extra {{{','.join(map(str, sorted(extra)))}}}")
            return False, f"Numbered placeholder mismatch: {', '.join(error_parts)}"
    
    # If translation has placeholders but source doesn't
    elif trans_unnamed > 0 or len(trans_numbered) > 0:
        return False, "Translation has placeholders but source doesn't"
    
    return True, ""


def verify_translation_file(file_path: str) -> List[Dict]:
    """
    Verify all translations in a Qt translation file.
    
    Returns:
        List of error dictionaries with details about invalid translations
    """
    try:
        tree = ET.parse(file_path)
        root = tree.getroot()
    except ET.ParseError as e:
        return [{"error": f"XML parsing error: {e}", "line": None}]
    except FileNotFoundError:
        return [{"error": f"File not found: {file_path}", "line": None}]
    
    errors = []
    
    # Find all message elements
    for message in root.findall('.//message'):
        source_elem = message.find('source')
        translation_elem = message.find('translation')
        location_elem = message.find('location')
        
        if source_elem is None or translation_elem is None:
            continue
        
        source_text = source_elem.text or ""
        translation_text = translation_elem.text or ""
       
        is_valid, error_msg = verify_placeholders(source_text, translation_text)
        
        if not is_valid:
            error_info = {
                "source": source_text,
                "translation": translation_text,
                "error": error_msg,
                "line": location_elem.get('line') if location_elem is not None else None,
                "filename": location_elem.get('filename') if location_elem is not None else None
            }
            errors.append(error_info)
    
    return errors


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <translation_file.ts>")
        sys.exit(1)
    
    file_path = sys.argv[1]
    
    if not Path(file_path).exists():
        print(f"Error: File '{file_path}' not found")
        sys.exit(1)
    
    print(f"Verifying placeholders in: {file_path}")
    
    errors = verify_translation_file(file_path)
    
    if not errors:
        print("All placeholders are valid.")
        sys.exit(0)
    
    print(f"Found {len(errors)} placeholder errors:")
    
    for i, error in enumerate(errors, 1):
        print(f"\nError {i}:")
        if error.get('filename') and error.get('line'):
            print(f"  Location: {error['filename']}:{error['line']}")
        print(f"  Source: {error['source']}")
        print(f"  Translation: {error['translation']}")
        print(f"  Issue: {error['error']}")
    
    sys.exit(1)


if __name__ == "__main__":
    main()