"""The device manifest (/vendor/ztos/manifest.toml) and the GSI's own description
(/usr/share/ztos/gsi.json), and the check between them (ztos-ial check).

A phone without a manifest still boots: every interface then gets its generic
backend, which is what a phone with full mainline support needs anyway.
"""
import json
import os
import pathlib
import tomllib

from . import INTERFACES

MANIFEST = 'vendor/ztos/manifest.toml'
GSI = 'usr/share/ztos/gsi.json'


def root_path(root, relative):
    return pathlib.Path(root or '/') / relative


def load(root=None):
    """The manifest as a dict, with [ial.X] filled in for every interface it
    does not name (backend = "generic")."""
    path = pathlib.Path(os.environ.get('ZTOS_IAL_MANIFEST') or root_path(root, MANIFEST))
    data = {}
    if path.exists():
        with path.open('rb') as stream:
            data = tomllib.load(stream)
    data.setdefault('device', {})
    data.setdefault('ial', {})
    data.setdefault('android', {})
    data['_path'] = str(path)
    data['_found'] = path.exists()
    for name in INTERFACES:
        entry = data['ial'].setdefault(name, {})
        entry.setdefault('backend', 'generic')
    return data


def gsi(root=None):
    path = root_path(root, GSI)
    if path.exists():
        return json.loads(path.read_text())
    return {'version': 'unknown', 'ial': {'required': [n for n, need in INTERFACES.items() if need],
                                          'interfaces': {n: 1 for n in INTERFACES}}}


def check(root=None):
    """Problems (hard) and notes (soft) of this manifest against this GSI."""
    manifest, system = load(root), gsi(root)
    problems, notes = [], []
    if not manifest['_found']:
        notes.append('no manifest at ' + manifest['_path'] + ': every interface uses its generic backend')
    required = system.get('ial', {}).get('required', [])
    versions = system.get('ial', {}).get('interfaces', {})
    for name, entry in manifest['ial'].items():
        if name not in INTERFACES:
            problems.append(f'[ial.{name}]: no such interface in this GSI')
            continue
        backend = entry.get('backend')
        if backend not in ('generic', 'vendor', 'service', 'none'):
            problems.append(f'[ial.{name}] backend = {backend!r}: generic, vendor, service or none')
        if backend == 'none' and name in required:
            problems.append(f'{name} is required by this GSI and cannot be "none"')
        if backend == 'vendor':
            module = entry.get('module')
            if not module:
                problems.append(f'[ial.{name}] backend = "vendor" needs module = "<path>.py"')
            elif not root_path(root, module.lstrip('/')).exists():
                problems.append(f'[ial.{name}] module {module} does not exist')
        if backend == 'service' and not entry.get('service'):
            problems.append(f'[ial.{name}] backend = "service" needs service = "<unit>"')
        wanted = entry.get('version', 1)
        served = versions.get(name, 1)
        if wanted > served:
            problems.append(f'{name}: the device layer wants version {wanted}, this GSI serves {served}')
    minimum = manifest.get('gsi', {}).get('min')
    if minimum and system.get('version') not in (None, 'unknown') and \
            _version(system['version']) < _version(minimum):
        problems.append(f'the device layer needs GSI {minimum} or newer, this is {system["version"]}')
    android = manifest.get('android', {})
    if android.get('binder', 'binderfs') == 'none':
        notes.append('no binder in this kernel: Waydroid stays hidden, ATL runs Android apps')
    return problems, notes


def _version(text):
    parts = []
    for piece in text.split('-')[0].split('.'):
        parts.append(int(piece) if piece.isdigit() else 0)
    return tuple(parts)
