#!/usr/bin/env python3
"""Cut ztOSMobile into the ztOS repositories (design/ztOSIAL.md, "Repositories").

    split.py preview OUT     copy today's files, committed or not, into OUT/<repo>/
                             to look at the result; no git, nothing changes here
    split.py git OUT         one new git repository per target in OUT/<repo>/,
                             with each file's history (git filter-repo); needs a
                             clean, committed ztOSMobile
    split.py list            which repository every file goes to

The rules are in map.txt next to this script. A path no rule matches stops the
split, so nothing is left behind by accident.
"""
import os
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
LICENCES = {'ztos-ial': '/usr/share/licenses/spdx/LGPL-2.1-or-later.txt'}


def rules():
    found = []
    for line in (HERE / 'map.txt').read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        repo, path = fields[0], fields[1]
        rename = fields[2] if len(fields) > 2 else None
        found.append((repo, path, rename))
    return found


def files(committed_only=False):
    command = ['git', '-C', str(ROOT), 'ls-files', '-z'] + ([] if committed_only else ['-co', '--exclude-standard'])
    out = subprocess.run(command, check=True, capture_output=True).stdout.decode()
    return sorted({f for f in out.split('\0') if f and (ROOT / f).exists()})


def place(path, table):
    """(repo, path in that repo) for one file, by the longest matching rule."""
    best = None
    for repo, rule, rename in table:
        matches = path == rule.rstrip('/') or (rule.endswith('/') and path.startswith(rule))
        if matches and (best is None or len(rule) > len(best[1])):
            best = (repo, rule, rename)
    if best is None:
        return None, None
    repo, rule, rename = best
    if repo == '-':
        return '-', None
    target = path
    if rename is not None:
        rest = path[len(rule):] if rule.endswith('/') else ''
        target = (rename.rstrip('/') + '/' + rest).lstrip('./') if rename not in ('./', '.') else rest
    return repo, target


def plan(committed_only=False):
    table, out, missing = rules(), {}, []
    for path in files(committed_only):
        repo, target = place(path, table)
        if repo is None:
            missing.append(path)
        elif repo != '-':
            out.setdefault(repo, []).append((path, target))
    if missing:
        sys.exit('no rule in map.txt for:\n  ' + '\n  '.join(missing[:50]) +
                 (f'\n  ... and {len(missing) - 50} more' if len(missing) > 50 else ''))
    return out


def licence(repo, directory):
    if (directory / 'LICENSE').exists():
        return
    source = LICENCES.get(repo)
    if source and os.path.exists(source):
        shutil.copy(source, directory / 'LICENSE')
    else:
        shutil.copy(ROOT / 'LICENSE', directory / 'LICENSE')


def preview(out):
    out = pathlib.Path(out)
    for repo, entries in plan().items():
        base = out / repo
        if base.exists():
            shutil.rmtree(base)
        for source, target in entries:
            destination = base / target
            destination.parent.mkdir(parents=True, exist_ok=True)
            if (ROOT / source).is_dir() and not (ROOT / source).is_symlink():
                destination.mkdir(exist_ok=True)    # a submodule: .gitmodules names it
                continue
            shutil.copy2(ROOT / source, destination, follow_symlinks=False)
        licence(repo, base)
        print(f'{repo:30} {len(entries):6} files -> {base}')


def git(out):
    if subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain', '--ignore-submodules=dirty'], capture_output=True, text=True).stdout.strip():
        sys.exit('ztOSMobile has uncommitted changes: commit them first, the split takes history')
    if not shutil.which('git-filter-repo'):
        sys.exit('needs git-filter-repo (pacman -S git-filter-repo)')
    out = pathlib.Path(out).resolve()   # filter-repo runs inside each clone
    for repo, entries in plan(committed_only=True).items():
        base = out / repo
        if base.exists():
            sys.exit(f'{base} exists: remove it or pick another folder')
        subprocess.run(['git', 'clone', '--no-local', '--quiet', str(ROOT), str(base)], check=True)
        paths = base / '.split-paths'
        paths.write_text(''.join(f'{source}\n' for source, _ in entries))
        command = ['git', 'filter-repo', '--force', '--paths-from-file', str(paths)]
        for source, target in entries:
            if source != target:
                command += ['--path-rename', f'{source}:{target}']
        subprocess.run(command, cwd=base, check=True)
        paths.unlink(missing_ok=True)
        if not (base / 'LICENSE').exists():
            licence(repo, base)
            subprocess.run(['git', 'add', 'LICENSE'], cwd=base, check=True)
            subprocess.run(['git', 'commit', '-q', '-m', 'Licence for this repository'], cwd=base, check=True)
        subprocess.run(['git', 'remote', 'remove', 'origin'], cwd=base, check=False)
        print(f'{repo:30} {len(entries):6} files, history kept -> {base}')


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('preview', 'git', 'list'):
        sys.exit(__doc__)
    if sys.argv[1] == 'list':
        for repo, entries in sorted(plan().items()):
            for source, target in entries:
                print(f'{repo}\t{source}\t{target}')
        return
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    {'preview': preview, 'git': git}[sys.argv[1]](sys.argv[2])


if __name__ == '__main__':
    main()
