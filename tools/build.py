#!/usr/bin/env python3
"""Build the mod and stage it as a RimWorld mod folder together with the native renderer."""
import argparse
import importlib.util
import platform
import re
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIMES = ('win-x64', 'linux-x64', 'osx-universal')
# Dev/ carries Smf.Dev.dll and its NuGet dependencies, not the mod's own assemblies.
NOT_DEV_FILES = {'Smf.Mod.dll', 'Smf.Shared.dll', 'AssetsTools.NET.dll', '0Harmony.dll'}


def host_runtime():
    system = platform.system()
    x64 = platform.machine().lower() in ('amd64', 'x86_64')

    if system == 'Darwin':
        return 'osx-universal'
    if system == 'Windows' and x64:
        return 'win-x64'
    if system == 'Linux' and x64:
        return 'linux-x64'
    raise ValueError('Pass --runtime; this host is not a supported build platform')


def load_packager():
    path = ROOT / 'tools/package-release.py'
    spec = importlib.util.spec_from_file_location('package_release', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(command):
    subprocess.run([str(part) for part in command], cwd=ROOT, check=True)


def check_rimworld():
    """Fail early with a clear message when the RimWorld assemblies are not where the csproj looks."""
    result = subprocess.run(
        ['dotnet', 'msbuild', str(ROOT / 'src/Smf.Mod/Smf.Mod.csproj'), '-getProperty:RimWorldManaged', '-nologo'],
        cwd=ROOT, check=True, capture_output=True, text=True)
    managed = Path(result.stdout.strip())

    if not (managed / 'Assembly-CSharp.dll').is_file():
        raise ValueError('RimWorld assemblies not found in ' + str(managed) + '; set RIMWORLD_PATH')


def build(args):
    if not re.fullmatch(r'[A-Za-z0-9._-]+', args.configuration):
        raise ValueError('Invalid configuration name')

    packager = load_packager()
    runtime = args.runtime or host_runtime()
    default_output = 'dist/dev/SimplyMoreFPS' if args.dev else 'dist/SimplyMoreFPS'
    requested_output = args.output or ROOT / default_output
    native = args.native_directory
    inputs = [ROOT / 'dist/build'] + ([native] if native is not None else [])
    output = packager.checked_output(requested_output, args.force, inputs)
    if native is None and runtime != host_runtime():
        raise ValueError('Pass --native-directory with the output of tools/' + packager.BUILD_SCRIPTS[runtime])

    if not args.reference_assemblies:
        check_rimworld()

    # Intermediates get a fresh folder per run so nothing stale is picked up.
    work = ROOT / 'dist/build' / uuid.uuid4().hex[:12]
    if native is None:
        native = work / 'native'
        run([sys.executable, ROOT / 'tools' / packager.BUILD_SCRIPTS[runtime], '--output', native])

    native_build = packager.validate_native_renderer(native, runtime)
    native_dir = Path(native_build['directory'])

    artifacts = work / 'managed'
    options = ['-c', args.configuration, '--artifacts-path', artifacts]
    if args.reference_assemblies:
        options.append('-p:UseRimWorldReferenceAssemblies=true')
    run(['dotnet', 'build', ROOT / 'src/Smf.Mod/Smf.Mod.csproj', *options])
    if args.dev:
        run(['dotnet', 'build', ROOT / 'src/Smf.Dev/Smf.Dev.csproj', *options])

    bin_dir = artifacts / 'bin'
    config = args.configuration.lower()
    copies = {}

    for name in ('Smf.Mod.dll', 'Smf.Shared.dll', 'AssetsTools.NET.dll'):
        copies['Assemblies/' + name] = bin_dir / 'Smf.Mod' / config / name
    for name in ('About/About.xml', 'About/Preview.png', 'README.md', 'LICENSE'):
        copies[name] = ROOT / name
    for path in packager.language_files(ROOT):
        copies[path.relative_to(ROOT).as_posix()] = path
    copies['ThirdParty/AssetsTools.NET-LICENSE.txt'] = ROOT / 'src/Smf.Mod/Rendering/Shaders/LICENSE.AssetsTools.NET'
    for name in native_build['files']:
        copies['Native/' + runtime + '/' + name] = native_dir / name
    copies['Native/' + runtime + '/Smf.Renderer.build.json'] = Path(native_build['receipt'])

    if args.dev:
        dev_bin = bin_dir / 'Smf.Dev' / config
        for path in dev_bin.glob('*.dll'):
            if path.name not in NOT_DEV_FILES:
                copies['Dev/' + path.name] = path
        if 'Dev/Smf.Dev.dll' not in copies:
            raise ValueError('Smf.Dev.dll missing from ' + str(dev_bin))

    for source in copies.values():
        packager.ordinary(source)

    # Replaced only now, so a failed build leaves the previous output alone.
    output = packager.replace_output(requested_output, args.force, [*inputs, *copies.values()])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()
    for relative, source in sorted(copies.items()):
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

    print('Staged ' + str(len(copies)) + ' files in ' + str(output))
    print('Native ' + runtime + ' build: ' + native_build['state'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration', default='Release')
    parser.add_argument('--runtime', choices=RUNTIMES, help='Default: this host')
    parser.add_argument('--dev', action='store_true', help='Also build src/Smf.Dev into Dev/')
    parser.add_argument('--reference-assemblies', action='store_true',
                        help='Build against pinned NuGet game references instead of a local RimWorld install')
    parser.add_argument('--output', type=Path,
                        help='New mod folder (default: dist/SimplyMoreFPS, or dist/dev/SimplyMoreFPS with --dev)')
    parser.add_argument('--force', action='store_true', help='Replace an existing output folder')
    parser.add_argument('--native-directory', type=Path,
                        help='Output folder of tools/build-renderer-<platform>.py; built here when omitted on the native host')

    args = parser.parse_args()
    try:
        build(args)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, 'Build failed: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
