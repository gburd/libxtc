#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (c) 2026, The XTC Project
"""
Sweep all source/doc files for non-ASCII characters and replace with ASCII equivalents.
"""
import os
import re
import sys
from pathlib import Path

# Directories to process
INCLUDE_DIRS = ['src', 'test', 'bench', 'docs', 'man', 'dist', 'examples']

# Directories to skip
SKIP_DIRS = ['.git', 'build_unix', 'build_cov', 'target', 'autom4te.cache']

# Files to skip
SKIP_FILES = ['LICENSE']

# Filenames that are typically binaries (no extension)
SKIP_BINARY_NAMES = ['bench']

# Binary extensions to skip
BINARY_EXTS = ['.rmeta', '.rlib', '.o', '.a', '.so', '.dylib', '.dll', '.exe', '.png', '.jpg', '.gif', '.lock']

# Replacement map for non-ASCII characters
REPLACEMENTS = {
    '\u2014': '--',       # em-dash
    '\u2013': '-',        # en-dash
    '\u2192': '->',       # right arrow
    '\u2190': '<-',       # left arrow
    '\u2713': '[OK]',     # checkmark
    '\u2717': '[X]',      # ballot X
    '\u2718': '[X]',      # heavy ballot X
    '\u2022': '*',        # bullet
    '\u201c': '"',        # left double quote
    '\u201d': '"',        # right double quote
    '\u2018': "'",        # left single quote
    '\u2019': "'",        # right single quote
    '\u2026': '...',      # ellipsis
    '\u00a0': ' ',        # non-breaking space
    '\u00d7': 'x',        # multiplication sign
    '\u00a7': '(S)',      # section sign
    '\u00b5': 'u',        # micro sign
    '\u00b7': '*',        # middle dot
    '\u2248': '~=',       # approximately equal
    '\u2261': '===',      # identical to
    '\u2264': '<=',       # less than or equal
    '\u2265': '>=',       # greater than or equal
    '\u2212': '-',        # minus sign
    '\u2076': '^6',       # superscript 6
    '\u2077': '^7',       # superscript 7
    '\u2500': '-',        # box drawing horizontal
    '\u2514': '\\-',      # box drawing corner
    '\u251c': '|-',       # box drawing tee
    '\u25e7': '[H]',      # square with left half black
    '\u25ef': '(O)',      # large circle
    '\u00b2': '^2',       # superscript 2
    '\u202f': ' ',        # narrow no-break space
    '\u2502': '|',        # box drawing vertical
    '\u250c': '+',        # box drawing corner top-left
    '\u2510': '+',        # box drawing corner top-right
    '\u2518': '+',        # box drawing corner bottom-right
    '\u2524': '|',        # box drawing tee right
}

def is_binary(filepath):
    """Check if file is binary."""
    path = Path(filepath)
    
    # Skip known binary names
    if path.name in SKIP_BINARY_NAMES:
        return True
    
    ext = path.suffix.lower()
    if ext in BINARY_EXTS:
        return True
    
    # Check for null bytes in first 8KB
    try:
        with open(filepath, 'rb') as f:
            chunk = f.read(8192)
            if b'\x00' in chunk:
                return True
    except:
        return True
    return False

def should_process(filepath):
    """Determine if file should be processed."""
    path = Path(filepath)
    
    # Skip directories
    for skip_dir in SKIP_DIRS:
        if skip_dir in path.parts:
            return False
    
    # Skip specific files
    if path.name in SKIP_FILES:
        return False
    
    # Skip binary files
    if is_binary(filepath):
        return False
    
    return True

def replace_non_ascii(content):
    """Replace non-ASCII characters with ASCII equivalents."""
    for old, new in REPLACEMENTS.items():
        content = content.replace(old, new)
    return content

def find_non_ascii(content):
    """Find all non-ASCII characters in content."""
    non_ascii = set()
    for i, char in enumerate(content):
        if ord(char) > 127:
            non_ascii.add((char, hex(ord(char)), i))
    return non_ascii

def process_file(filepath, dry_run=False):
    """Process a single file, return (modified, char_count, details)."""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            original = f.read()
    except Exception as e:
        return False, 0, f"Error reading: {e}"
    
    # Find non-ASCII before replacement
    non_ascii_before = find_non_ascii(original)
    if not non_ascii_before:
        return False, 0, None
    
    # Apply replacements
    modified = replace_non_ascii(original)
    
    # Check for remaining non-ASCII
    non_ascii_after = find_non_ascii(modified)
    
    if not dry_run and modified != original:
        try:
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(modified)
        except Exception as e:
            return False, 0, f"Error writing: {e}"
    
    char_count = len(non_ascii_before) - len(non_ascii_after)
    remaining = []
    for char, hex_val, pos in non_ascii_after:
        remaining.append(f"  {hex_val} ('{char}') at pos {pos}")
    
    details = None
    if remaining:
        details = f"Remaining non-ASCII:\n" + "\n".join(remaining[:5])
        if len(remaining) > 5:
            details += f"\n  ... and {len(remaining) - 5} more"
    
    return True, len(non_ascii_before), details

def main():
    dry_run = '--dry-run' in sys.argv
    verbose = '--verbose' in sys.argv or '-v' in sys.argv
    
    root = Path(__file__).parent.parent
    os.chdir(root)
    
    total_files = 0
    modified_files = 0
    total_chars = 0
    errors = []
    
    # Process directories
    for dir_name in INCLUDE_DIRS:
        dir_path = root / dir_name
        if not dir_path.exists():
            continue
        
        for filepath in dir_path.rglob('*'):
            if filepath.is_file() and should_process(str(filepath)):
                total_files += 1
                was_modified, char_count, details = process_file(str(filepath), dry_run)
                
                if was_modified:
                    modified_files += 1
                    total_chars += char_count
                    if verbose:
                        print(f"{'[DRY-RUN] ' if dry_run else ''}Modified: {filepath.relative_to(root)} ({char_count} chars)")
                    if details:
                        errors.append((str(filepath.relative_to(root)), details))
    
    # Process root files
    for root_file in ['AUTHORS', 'README.md', 'PLAN.md', 'AGENTS.md']:
        filepath = root / root_file
        if filepath.exists() and should_process(str(filepath)):
            total_files += 1
            was_modified, char_count, details = process_file(str(filepath), dry_run)
            
            if was_modified:
                modified_files += 1
                total_chars += char_count
                if verbose:
                    print(f"{'[DRY-RUN] ' if dry_run else ''}Modified: {root_file} ({char_count} chars)")
                if details:
                    errors.append((root_file, details))
    
    # Summary
    print(f"\n{'=' * 60}")
    print(f"ASCII Sweep {'(DRY RUN)' if dry_run else 'Complete'}")
    print(f"{'=' * 60}")
    print(f"Files scanned:  {total_files}")
    print(f"Files modified: {modified_files}")
    print(f"Characters replaced: {total_chars}")
    
    if errors:
        print(f"\nWarnings ({len(errors)} files with remaining non-ASCII):")
        for filepath, details in errors[:10]:
            print(f"\n{filepath}:")
            print(details)
        if len(errors) > 10:
            print(f"\n... and {len(errors) - 10} more files with warnings")
    
    return 0 if not errors else 1

if __name__ == '__main__':
    sys.exit(main())
