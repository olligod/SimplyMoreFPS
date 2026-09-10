#!/usr/bin/env python3
"""Validate native renderer builds and pack the release zips."""
import argparse
import hashlib
import importlib.util
import json
import os
import plistlib
import shutil
import stat
import struct
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIMES = ('win-x64', 'linux-x64', 'osx-universal')
RECEIPT_NAME = 'Smf.Renderer.build.json'
BUILD_SCRIPTS = {
    'win-x64': 'build-renderer-windows.py',
    'linux-x64': 'build-renderer-linux.py',
    'osx-universal': 'build-renderer-macos.py',
}
NATIVE_FILES = {
    'win-x64': (
        'Smf.Renderer.dll',
        'Smf.Camera.dll',
        'MinHook-LICENSE.txt',
        'DOTNET-LICENSE.txt',
        'DOTNET-THIRD-PARTY-NOTICES.txt',
    ),
    'linux-x64': (
        'libSmf.Renderer.so',
        'libSmf.Camera.so',
        'DOTNET-LICENSE.txt',
        'DOTNET-THIRD-PARTY-NOTICES.txt',
    ),
    'osx-universal': (
        'Smf.Renderer.bundle/Contents/Info.plist',
        'Smf.Renderer.bundle/Contents/MacOS/Smf.Renderer',
        'Smf.Renderer.bundle/Contents/_CodeSignature/CodeResources',
        'libSmf.Camera.dylib',
        'DOTNET-LICENSE.txt',
        'DOTNET-THIRD-PARTY-NOTICES.txt',
    ),
}
MOD_FILES = (
    'About/About.xml',
    'About/Preview.png',
    'Assemblies/Smf.Mod.dll',
    'Assemblies/Smf.Shared.dll',
    'Assemblies/AssetsTools.NET.dll',
    'ThirdParty/AssetsTools.NET-LICENSE.txt',
    'README.md',
    'LICENSE',
)
CPU_TYPE_X86_64 = 0x01000007
CPU_TYPE_ARM64 = 0x0100000C
MH_DYLIB = 6
MH_BUNDLE = 8
# Fixed so the same inputs give byte-identical zips.
ZIP_DATE = (2026, 9, 7, 0, 0, 0)


def checked_output(path, force, inputs=()):
    """Keep replacement away from sources, inputs and redirected filesystem paths."""
    original = Path(path).absolute()
    output = Path(os.path.abspath(original))
    for candidate in (original, *original.parents, output, *output.parents):
        try:
            info = candidate.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Output path contains a symlink or reparse point: ' + str(candidate))

    root = ROOT.resolve()
    if output == output.parent or output == root or output in root.parents:
        raise ValueError('Output would replace the repository or its parent: ' + str(output))
    protected = [root / name for name in ('src', 'tools', 'tests', 'About', 'Languages', '.git', '.github', '.vscode')]
    protected += [Path(source).resolve() for source in inputs]
    for source in protected:
        if output == source or output in source.parents or source in output.parents:
            raise ValueError('Output overlaps a source or input: ' + str(source))
    if output.exists():
        if not output.is_dir():
            raise ValueError('Output is not a directory: ' + str(output))
        if not force:
            raise ValueError('Output already exists; pass --force: ' + str(output))
    return output


def replace_output(path, force, inputs=()):
    output = checked_output(path, force, inputs)
    if output.exists():
        # Do not traverse nested junctions, symlinks or another repository during replacement.
        pending = [output]
        while pending:
            with os.scandir(pending.pop()) as entries:
                for entry in entries:
                    info = entry.stat(follow_symlinks=False)
                    if entry.name == '.git' or stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
                        raise ValueError('Output contains a repository or redirected path: ' + entry.path)
                    if stat.S_ISDIR(info.st_mode):
                        pending.append(entry.path)
        shutil.rmtree(output)
    return output


def ordinary(path):
    """Read a regular file. Symlinks anywhere in the path are refused."""
    path = Path(path)
    if not path.is_file() or any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError('Expected a regular file: ' + str(path))
    return path.read_bytes()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_module(script):
    """Import a sibling script from tools/ by file name."""
    path = Path(__file__).with_name(script)
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def language_files(root):
    """All translation XML files under Languages/, checked to be well formed."""
    root = Path(root)
    ordinary(root / 'Languages/English/Keyed/SimplyMoreFPS.xml')
    files = sorted((root / 'Languages').rglob('*.xml'))

    for path in files:
        relative = path.relative_to(root / 'Languages')
        if len(relative.parts) < 3 or relative.parts[1] not in ('Keyed', 'DefInjected'):
            raise ValueError('Unexpected translation path: ' + str(relative))
        document = ET.fromstring(ordinary(path))
        if document.tag != 'LanguageData':
            raise ValueError('Expected LanguageData in ' + str(relative))
        keys = [child.tag for child in document]
        if len(keys) != len(set(keys)):
            raise ValueError('Duplicate translation key in ' + str(relative))

    return files


def source_files_for_native(runtime):
    """Every file a native build depends on. The build receipt must list exactly these."""
    if runtime not in RUNTIMES:
        raise ValueError('Unsupported native runtime: ' + runtime)

    files = [ROOT / 'Directory.Build.props', ROOT / 'tools' / BUILD_SCRIPTS[runtime],
             ROOT / 'tools/native_build.py', ROOT / 'tools/package-release.py']
    if runtime == 'osx-universal':
        files.append(ROOT / 'tools/macos-signature-check.py')
    files += sorted((ROOT / 'src/Smf.Shared').glob('*.cs'))
    if (ROOT / 'global.json').is_file():
        files.append(ROOT / 'global.json')

    for folder in (ROOT / 'src/Smf.Renderer', ROOT / 'src/Smf.Camera'):
        found = []
        for path in folder.rglob('*'):
            parts = path.relative_to(folder).parts
            if not path.is_file() or set(parts) & {'bin', 'obj', '__pycache__'}:
                continue
            # The Windows build does not snapshot the other two renderer ports.
            if runtime == 'win-x64' and folder.name == 'Smf.Renderer' and parts[0] in ('linux', 'mac'):
                continue
            found.append(path)

        if not found:
            raise ValueError('No native sources under ' + str(folder))
        files += found

    for path in files:
        ordinary(path)
    return sorted(set(files))


def find_receipt(directory):
    """Staged packages keep the receipt beside the files; raw build folders keep it at the top."""
    staged = directory / RECEIPT_NAME
    if staged.is_file():
        return staged
    raw = directory.parent.parent / 'build-receipt.json'
    if raw.is_file():
        return raw
    raise ValueError('No build receipt found for ' + str(directory))


def receipt_rows(receipt, key):
    """The receipt's {path, sha256, ...} entries under key, indexed by path."""
    rows = receipt.get(key)
    if not isinstance(rows, list):
        raise ValueError('Receipt has no ' + key + ' list')

    by_path = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get('path'), str):
            raise ValueError('Malformed receipt ' + key + ' entry')
        if row['path'] in by_path:
            raise ValueError('Duplicate receipt ' + key + ' entry: ' + row['path'])
        by_path[row['path']] = row

    return by_path


def verify_hash(row, path):
    """The file at path still matches its receipt entry."""
    data = ordinary(path)
    digest = sha256(data)
    if str(row.get('sha256', '')).lower() != digest or row.get('bytes', len(data)) != len(data):
        raise ValueError('Hash mismatch: ' + str(path))
    return digest


def verify_pe_dll(data, name):
    """A Windows x64 PE32+ DLL."""
    if len(data) < 64 or data[:2] != b'MZ':
        raise ValueError('Not a PE file: ' + name)
    header = struct.unpack_from('<I', data, 60)[0]
    if header > len(data) - 26 or data[header:header + 4] != b'PE\0\0':
        raise ValueError('Not a PE file: ' + name)

    machine = struct.unpack_from('<H', data, header + 4)[0]
    characteristics = struct.unpack_from('<H', data, header + 22)[0]
    magic = struct.unpack_from('<H', data, header + 24)[0]
    if machine != 0x8664 or not characteristics & 0x2000 or magic != 0x20B:
        raise ValueError('Expected a Windows x64 DLL: ' + name)


def verify_elf(data, name):
    """A Linux x86_64 little-endian ELF shared library."""
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('Not a 64-bit little-endian ELF file: ' + name)
    file_type, machine, version = struct.unpack_from('<HHI', data, 16)
    if (file_type, machine, version) != (3, 62, 1):
        raise ValueError('Expected an x86_64 ELF shared library: ' + name)


def resource_seal_matches(record, data):
    """One CodeResources entry hashes the given file, in the old sha1 form or the hash2 dict."""
    sha1 = hashlib.sha1(data).digest()
    if isinstance(record, bytes):
        return record == sha1
    if not isinstance(record, dict) or record.get('symlink') or record.get('cdhash'):
        return False
    return record.get('hash2') == hashlib.sha256(data).digest() or record.get('hash') == sha1


def verify_mac_bundle(directory):
    """Both Mach-O files are signed universal binaries and the bundle seals only Info.plist."""
    macho = load_module('macos-signature-check.py')
    contents = directory / 'Smf.Renderer.bundle/Contents'
    info_bytes = ordinary(contents / 'Info.plist')
    resources_bytes = ordinary(contents / '_CodeSignature/CodeResources')
    info = plistlib.loads(info_bytes)
    if info.get('CFBundleExecutable') != 'Smf.Renderer' or info.get('CFBundlePackageType') != 'BNDL':
        raise ValueError('Expected the Smf.Renderer loadable bundle')

    # Signature special slot 1 is Info.plist and slot 3 is CodeResources.
    checks = (
        ('Smf.Renderer.bundle/Contents/MacOS/Smf.Renderer', MH_BUNDLE, {1: info_bytes, 3: resources_bytes}),
        ('libSmf.Camera.dylib', MH_DYLIB, None),
    )

    signatures = {}
    for name, file_type, external in checks:
        slices = macho.inspect_bytes(ordinary(directory / name), external)
        if len(slices) != 2 or {s['cpu_type'] for s in slices} != {CPU_TYPE_X86_64, CPU_TYPE_ARM64}:
            raise ValueError('Expected signed x86_64 and arm64 slices: ' + name)
        if any(s['file_type'] != file_type for s in slices):
            raise ValueError('Unexpected Mach-O file type: ' + name)
        if any(d['unchecked_external_slots'] for s in slices for d in s['directories']):
            raise ValueError('Signature seals resources that were not checked: ' + name)
        signatures[name] = slices

    resources = plistlib.loads(resources_bytes)
    for key in ('files', 'files2'):
        for relative, record in resources.get(key, {}).items():
            if relative != 'Info.plist':
                raise ValueError('Unexpected sealed bundle resource: ' + relative)
            if not resource_seal_matches(record, info_bytes):
                raise ValueError('Sealed Info.plist hash mismatch')

    return signatures


def validate_native_renderer(directory, runtime):
    """Check a native build against its receipt, the current sources and the binary format."""
    if runtime not in RUNTIMES:
        raise ValueError('Unsupported native runtime: ' + runtime)

    directory = Path(directory).absolute()
    if (directory / 'Native' / runtime).is_dir():
        directory = directory / 'Native' / runtime

    receipt_path = find_receipt(directory)
    receipt_bytes = ordinary(receipt_path)
    receipt = json.loads(receipt_bytes)
    if not isinstance(receipt, dict) or receipt.get('version') != 1 or receipt.get('platform') != runtime:
        raise ValueError('Expected a version 1 build receipt for ' + runtime + ' at ' + str(receipt_path))
    state = receipt.get('state')
    if state not in ('built-not-tested', 'built-cpu-tested'):
        raise ValueError('Native build did not complete: ' + str(state))
    steps = receipt.get('steps')
    if not isinstance(steps, list) or not steps:
        raise ValueError('Receipt has no build steps')
    if any(not isinstance(step, dict) or step.get('exitCode') != 0 for step in steps):
        raise ValueError('A native build step failed')

    expected = {p.relative_to(ROOT).as_posix(): p for p in source_files_for_native(runtime)}
    sources = receipt_rows(receipt, 'sources')
    if set(sources) != set(expected):
        missing = sorted(set(expected) - set(sources))[:5]
        extra = sorted(set(sources) - set(expected))[:5]
        raise ValueError('Receipt sources differ from the tree, rebuild the renderer. '
                         'Missing: ' + str(missing) + ' extra: ' + str(extra))

    source_hashes = {name: verify_hash(sources[name], path) for name, path in expected.items()}

    files = NATIVE_FILES[runtime]
    prefix = 'Native/' + runtime + '/'
    binaries = [name for name in files if not name.endswith('.txt')]
    licenses = [name for name in files if name.endswith('.txt')]
    outputs = receipt_rows(receipt, 'outputs')
    notices = receipt_rows(receipt, 'notices')
    if set(outputs) != {prefix + n for n in binaries} or set(notices) != {prefix + n for n in licenses}:
        raise ValueError('Receipt must list exactly the native binaries and license notices')

    output_hashes = {}
    for name in binaries:
        output_hashes[name] = verify_hash(outputs[prefix + name], directory / name)
    for name in licenses:
        output_hashes[name] = verify_hash(notices[prefix + name], directory / name)

    signatures = None
    if runtime == 'win-x64':
        for name in binaries:
            verify_pe_dll(ordinary(directory / name), name)
    elif runtime == 'linux-x64':
        for name in binaries:
            verify_elf(ordinary(directory / name), name)
    else:
        signatures = verify_mac_bundle(directory)

    return {
        'platform': runtime,
        'directory': str(directory.resolve()),
        'files': list(files),
        'receipt': str(receipt_path.resolve()),
        'receipt_sha256': sha256(receipt_bytes),
        'state': state,
        'source_sha256': source_hashes,
        'output_sha256': output_hashes,
        'signatures': signatures,
    }


def mod_version(about_xml):
    """The modVersion from About.xml, limited to characters that are safe in a file name."""
    metadata = ET.fromstring(about_xml)
    versions = metadata.findall('modVersion')
    version = (versions[0].text or '').strip() if len(versions) == 1 else ''
    allowed = set('0123456789abcdefghijklmnopqrstuvwxyz.-')

    if metadata.tag != 'ModMetaData' or not version or set(version) - allowed:
        raise ValueError('About.xml needs exactly one modVersion like 1.2.3')
    return version


def mod_entries(mod):
    """Zip entries for the mod itself: only allowlisted files, never Dev tooling."""
    if (mod / 'Dev').exists():
        raise ValueError('Refusing a --dev build; run tools/build.py without --dev')

    entries = {}
    for name in MOD_FILES:
        entries['SimplyMoreFPS/' + name] = ordinary(mod / name)
    for path in language_files(mod):
        entries['SimplyMoreFPS/' + path.relative_to(mod).as_posix()] = ordinary(path)
    return entries


def native_entries(directory, runtime):
    """Zip entries for one validated native build, with its validation result."""
    result = validate_native_renderer(directory, runtime)
    entries = {}
    for name in result['files']:
        entries['SimplyMoreFPS/Native/' + runtime + '/' + name] = ordinary(Path(result['directory']) / name)
    return result, entries


def write_zip(path, entries):
    with zipfile.ZipFile(path, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, data in sorted(entries.items()):
            info = zipfile.ZipInfo(name, date_time=ZIP_DATE)
            # Unix attributes so the mode bits survive on Linux and macOS.
            info.create_system = 3
            executable = name.endswith(('.so', '.dylib', '/MacOS/Smf.Renderer'))
            info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)

    with zipfile.ZipFile(path) as archive:
        if set(archive.namelist()) != set(entries) or archive.testzip() is not None:
            raise ValueError('Zip verification failed: ' + str(path))


def package(args):
    runtimes = tuple(args.runtimes or RUNTIMES)
    if len(set(runtimes)) != len(runtimes):
        raise ValueError('Duplicate --runtimes entry')

    mod = args.mod.absolute()
    base = mod_entries(mod)
    version = mod_version(base['SimplyMoreFPS/About/About.xml'])
    if args.version is not None and args.version != version:
        raise ValueError('--version ' + args.version + ' does not match About.xml (' + version + ')')

    natives_root = args.natives or mod / 'Native'
    natives = {}
    for runtime in runtimes:
        natives[runtime] = native_entries(natives_root / runtime, runtime)

    bundles = [(runtime, [runtime]) for runtime in runtimes]
    if set(runtimes) == set(RUNTIMES):
        bundles.append(('universal', list(RUNTIMES)))

    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    zips = [output / ('SimplyMoreFPS-' + version + '-' + label + '.zip') for label, _ in bundles]
    for path in zips + [output / 'manifest.json', output / 'SHA256SUMS.txt']:
        if path.exists():
            raise ValueError('Already exists, pick a fresh --output: ' + str(path))

    manifest = {'version': version, 'native': {}, 'archives': []}
    for runtime, (result, _) in natives.items():
        manifest['native'][runtime] = {'state': result['state'], 'receipt_sha256': result['receipt_sha256']}

    for path, (_, included) in zip(zips, bundles):
        entries = dict(base)
        for runtime in included:
            entries.update(natives[runtime][1])
        write_zip(path, entries)
        data = path.read_bytes()
        manifest['archives'].append({'file': path.name, 'bytes': len(data), 'sha256': sha256(data), 'runtimes': included})
        print(path)

    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    sums = ''.join(a['sha256'] + '  ' + a['file'] + '\n' for a in manifest['archives'])
    (output / 'SHA256SUMS.txt').write_text(sums)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mod', type=Path, help='Mod folder staged by tools/build.py')
    parser.add_argument('--natives', type=Path,
                        help='Folder with one native build per runtime (default: <mod>/Native)')
    parser.add_argument('--output', type=Path, help='Folder for the zips, manifest.json and SHA256SUMS.txt')
    parser.add_argument('--version', help='Fail unless About.xml has this modVersion')
    parser.add_argument('--runtimes', nargs='+', choices=RUNTIMES, help='Default: all three plus a universal zip')
    parser.add_argument('--verify-native', type=Path, metavar='DIR',
                        help='Only validate one native build and print the result')
    parser.add_argument('--runtime', choices=RUNTIMES, default='win-x64', help='Runtime for --verify-native')

    args = parser.parse_args()
    try:
        if args.verify_native is not None:
            print(json.dumps(validate_native_renderer(args.verify_native, args.runtime), indent=2))
        elif args.mod is None or args.output is None:
            parser.error('--mod and --output are required')
        else:
            package(args)
    except (ValueError, OSError, KeyError, TypeError, ET.ParseError) as error:
        parser.exit(1, 'Packaging failed: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
