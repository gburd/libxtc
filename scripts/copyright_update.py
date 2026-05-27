#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (c) 2026, The XTC Project
"""
Update copyright headers to remove "All rights reserved" and use consistent format.
"""
import os
import re
import sys
from pathlib import Path

# Pattern to match the old copyright line
OLD_PATTERNS = [
    r'Copyright \(c\) 2026, The XTC Project -- All rights reserved\.',
    r'Copyright \(c\) 2026, The XTC Project - All rights reserved\.',
]

NEW_COPYRIGHT = 'Copyright (c) 2026, The XTC Project'

def process_file(filepath, dry_run=False):
    """Process a single file, return True if modified."""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            content = f.read()
    except Exception as e:
        print(f"Error reading {filepath}: {e}")
        return False
    
    modified = content
    for pattern in OLD_PATTERNS:
        modified = re.sub(pattern, NEW_COPYRIGHT, modified)
    
    if modified != content:
        if not dry_run:
            try:
                with open(filepath, 'w', encoding='utf-8') as f:
                    f.write(modified)
            except Exception as e:
                print(f"Error writing {filepath}: {e}")
                return False
        return True
    return False

def main():
    dry_run = '--dry-run' in sys.argv
    
    root = Path(__file__).parent.parent
    os.chdir(root)
    
    # Directories to process
    dirs = ['src', 'test', 'bench', 'docs', 'man', 'dist', 'examples']
    
    # Extensions to process
    extensions = ['.c', '.h', '.S', '.sh', '.md', '.3', '.7', '.ac', '.am', '.in', '.py', '.rs', '.erl']
    
    # Also process Makefile files
    makefile_names = ['Makefile']
    
    count = 0
    for dir_name in dirs:
        dir_path = root / dir_name
        if not dir_path.exists():
            continue
        for filepath in dir_path.rglob('*'):
            if filepath.is_file() and (filepath.suffix in extensions or filepath.name in makefile_names):
                if process_file(str(filepath), dry_run):
                    count += 1
                    print(f"{'[DRY-RUN] ' if dry_run else ''}Updated: {filepath.relative_to(root)}")
    
    # Process root files
    for root_file in ['AUTHORS', 'README.md', 'PLAN.md']:
        filepath = root / root_file
        if filepath.exists():
            if process_file(str(filepath), dry_run):
                count += 1
                print(f"{'[DRY-RUN] ' if dry_run else ''}Updated: {root_file}")
    
    print(f"\n{'=' * 40}")
    print(f"Copyright update {'(DRY RUN)' if dry_run else 'complete'}")
    print(f"Files updated: {count}")
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
