#!/usr/bin/env python3
"""
firered-vad-esp32-p4_backup.py - create project backup zip and copy to central archive
Usage: python firered-vad-esp32-p4_backup.py [--project-root PATH] [--archive-root PATH] [--exclude PATH ...] [--dry-run]
"""
from __future__ import annotations
import argparse
import os
import sys
import zipfile
import datetime
import shutil
from typing import List


def find_project_root(script_path: str) -> str:
    return os.path.abspath(os.path.join(os.path.dirname(script_path), '..', '..', '..'))


def normalize_excludes(excludes: List[str]) -> List[str]:
    out = []
    for ex in excludes:
        ex = ex.replace('/', os.sep).replace('\\', os.sep).strip(os.sep)
        out.append(ex)
    return out


def is_excluded(abs_path: str, project_root: str, excludes: List[str]) -> bool:
    rel = os.path.relpath(abs_path, project_root)
    rel = rel.replace('\\', '/').replace('\\', '/')
    for ex in excludes:
        ex_norm = ex.replace('\\', '/').replace('\\', '/').rstrip('/')
        if rel == ex_norm or rel.startswith(ex_norm + '/'):
            return True
    return False


def is_file_excluded(filename: str, file_excludes: List[str]) -> bool:
    lower_name = filename.lower()
    for pattern in file_excludes:
        if lower_name.endswith(pattern.lower()):
            return True
    return False


def make_zip(project_root: str, dest_zip: str, excludes: List[str], file_excludes: List[str]) -> str:
    with zipfile.ZipFile(dest_zip, 'w', compression=zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(project_root):
            if is_excluded(root, project_root, excludes):
                dirs[:] = []
                continue
            rel_root = os.path.relpath(root, project_root)
            if rel_root == '.':
                rel_root = ''
            for f in files:
                if is_file_excluded(f, file_excludes):
                    continue
                absf = os.path.join(root, f)
                if os.path.abspath(absf) == os.path.abspath(dest_zip):
                    continue
                arcname = os.path.normpath(os.path.join(rel_root, f))
                zf.write(absf, arcname)
    return dest_zip


def main() -> int:
    parser = argparse.ArgumentParser(description='Create a project ZIP backup and copy to central archive.')
    parser.add_argument('--project-root', help='Project root path (defaults to three levels up from this script).', default=None)
    parser.add_argument('--archive-root', help='Central archive root (overrides auto-detection).', default=None)
    parser.add_argument('--exclude', action='append', help='Relative paths to exclude (can be provided multiple times).', default=[])
    parser.add_argument('--dry-run', action='store_true', help='Show what would be done without creating files.')
    args = parser.parse_args()

    script_path = __file__
    project_root = args.project_root or find_project_root(script_path)
    project_root = os.path.abspath(project_root)

    if not os.path.isdir(project_root):
        print(f'Project root not found: {project_root}', file=sys.stderr)
        return 2

    project_name = os.path.basename(project_root)
    # Remove version number from project name (e.g., "project-1.0.0" -> "project")
    import re
    project_name_no_version = re.sub(r'-\d+\.\d+\.\d+$', '', project_name)
    safe_project = project_name_no_version.replace(' ', '_')

    default_excludes = ['models', 'docs', 'build', 'install', 'managed_components', 'dependencies', 'tools/backup', 'backup', 'backups', 'logs', '.git', '.vs', '.vscode', '__pycache__']
    default_file_excludes = ['.md', '.log']
    excludes = default_excludes + (args.exclude or [])
    excludes = normalize_excludes(excludes)

    ts = datetime.datetime.now().strftime('%Y-%m-%d_%H-%M-%S')
    zip_filename = f'{ts}_{safe_project}.zip'

    project_backup_dir = os.path.join(project_root, 'tools', 'backup', 'backups')
    os.makedirs(project_backup_dir, exist_ok=True)
    project_zip_path = os.path.join(project_backup_dir, zip_filename)

    print(f'Project: {project_name}')
    print(f'Project root: {project_root}')
    print(f'Creating archive: {zip_filename}')
    print(f'Local backup dir: {project_backup_dir}')

    if args.dry_run:
        print('Dry run: no files will be created.')
        return 0

    make_zip(project_root, project_zip_path, excludes, default_file_excludes)

    print('Backup completed.')
    print(f'- Local: {project_zip_path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
