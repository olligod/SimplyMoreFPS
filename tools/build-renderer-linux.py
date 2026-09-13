#!/usr/bin/env python3
"""Build the Linux x64 renderer and camera kernel, with optional CPU tests.

Requires c++, binutils, .NET 10 NativeAOT and the GL, X11, Xext and Xi development
libraries. Nothing is installed and no game is started. --plan only reports readiness.
"""
import argparse
import json
import os
import platform
import shutil
import sys
import time
from pathlib import Path

# CI also imports these scripts by path, without tools/ on its module search path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_build import BuildLog, RECEIPT_NAME, load_package, snapshot_sources, write_receipt
from native_build import file_digest as digest, file_record as record

ROOT = Path(__file__).resolve().parents[1]
RENDERER_DIR = ROOT / 'src/Smf.Renderer/linux'

RENDERER_SOURCES = (
    'linux_session.cpp',
    'linux_glx.cpp',
    'scene_compositor.cpp',
    'glx_source_router.cpp',
    'linux_unity_swap_slot.cpp',
    'camera_bridge.cpp',
    'camera_control.cpp',
    'original_window.cpp',
    'x_error_ledger.cpp',
)
POLICY_TESTS = (
    'core_tests.cpp',
    'integration_policy_tests.cpp',
    'original_window_tests.cpp',
    'borrowed_display_tests.cpp',
    'x_error_ledger_tests.cpp',
)
# Links the real bridge against the built camera kernel, so it is not a plain policy test.
BRIDGE_TEST = 'camera_bridge_trajectory_tests.cpp'
# The camera ABI check lives with the kernel, outside linux/.
CAMERA_DRIVER = 'src/Smf.Camera/tests/native_driver.cpp'
CAMERA_REFERENCE = 'src/Smf.Camera/tests/Reference.csproj'
# Headers pulled in from outside linux/; they must be part of the snapshot.
SHARED_HEADERS = (
    'src/Smf.Renderer/common/camera_packets.h',
    'src/Smf.Renderer/common/selection_overlay.h',
    'src/Smf.Renderer/common/wheel_modifiers.h',
    'src/Smf.Renderer/common/scene_packets.h',
    'src/Smf.Renderer/common/scene_snapshot.h',
    'src/Smf.Renderer/common/projection_math.h',
    'src/Smf.Camera/camera_kernel.h',
)
NOTICES = ('DOTNET-LICENSE.txt', 'DOTNET-THIRD-PARTY-NOTICES.txt')

SESSION_EXPORTS = ('start', 'command', 'content_fence', 'ack', 'status', 'pre_gui', 'frame', 'native_frame',
                   'cancel', 'render_event', 'poll_joined', 'clock_now', 'clock_frequency', 'quit',
                   'platform_status', 'find_original_window')
BRIDGE_EXPORTS = ('init', 'publish', 'revoke', 'desired', 'status', 'now')
CONTROL_EXPORTS = ('policy', 'impulse', 'status', 'wheel_status')
RENDERER_EXPORTS = (tuple('smf_session_' + name for name in SESSION_EXPORTS)
                    + tuple('smf_camera_bridge_' + name for name in BRIDGE_EXPORTS)
                    + tuple('smf_camera_control_' + name for name in CONTROL_EXPORTS)
                    + ('smf_selection_publish',))
CAMERA_EXPORTS = tuple('smf_camera_' + name for name in ('create', 'adopt', 'configure', 'step', 'release'))

TOOLS = ('c++', 'nm', 'readelf', 'dotnet')
LINK_LIBRARIES = ('GL', 'X11', 'Xext', 'Xi', 'dl')


def load_package_module():
    return load_package(ROOT, 'smf_linux_package')


def required_files(with_tests):
    files = [RENDERER_DIR / name for name in RENDERER_SOURCES]
    files += [ROOT / path for path in SHARED_HEADERS]
    if with_tests:
        files += [RENDERER_DIR / 'tests' / name for name in (*POLICY_TESTS, BRIDGE_TEST)]
        files += [ROOT / CAMERA_DRIVER, ROOT / CAMERA_REFERENCE]
    return files


def relative(path):
    return path.relative_to(ROOT).as_posix()


class Build:
    def __init__(self, out, snapshot, tool, receipt):
        self.out = out
        self.snapshot = snapshot
        self.tool = tool
        self.receipt = receipt
        self.env = dict(os.environ, DOTNET_PROCESSOR_COUNT='2', LC_ALL='C')
        self.log_runner = BuildLog(out, self.env, receipt)
        self.renderer = snapshot / 'src/Smf.Renderer/linux'
        self.camera = snapshot / 'src/Smf.Camera'
        self.native = out / 'Native/linux-x64'
        self.compile = [tool['c++'], '-std=c++17', '-O2', '-Wall', '-Wextra', '-pthread']

    def run(self, label, command):
        return self.log_runner.run(label, command)

    def managed_options(self):
        artifacts = self.out / 'managed'
        return ['-c', 'Release', '-m:1', '--disable-build-servers', '-p:UseSharedCompilation=false',
                '-p:BuildInParallel=false', '--artifacts-path', artifacts,
                '-p:PathMap=' + str(self.snapshot) + '=/_/source']

    def build_renderer(self):
        sources = [self.renderer / name for name in RENDERER_SOURCES]
        libraries = ['-l' + name for name in LINK_LIBRARIES]
        self.run('renderer', [*self.compile, '-g', '-fPIC', '-fvisibility=hidden', '-shared', '-DXTHREADS',
                              *sources, '-o', self.native / 'libSmf.Renderer.so', *libraries, '-Wl,-z,defs'])

    def build_camera(self, package):
        publish = self.out / 'camera-publish'
        self.run('camera', [self.tool['dotnet'], 'publish', self.camera / 'Smf.Camera.csproj', '-r', 'linux-x64',
                            *self.managed_options(), '-p:IlcSingleThreaded=true', '-o', publish])

        # NativeAOT names the Linux library after AssemblyName without a lib prefix.
        produced = publish / 'Smf.Camera.so'
        package.ordinary(produced)
        shutil.copy2(produced, self.native / 'libSmf.Camera.so')

    def check_library(self, package, name, expected_exports):
        library = self.native / name
        package.verify_elf(package.ordinary(library), name)
        self.run(name + '-header', [self.tool['readelf'], '-h', library])
        symbols = self.run(name + '-exports', [self.tool['nm'], '-D', '--defined-only', library])

        exported = set()
        for line in symbols.splitlines():
            parts = line.split()
            if parts:
                exported.add(parts[-1].split('@')[0])

        missing = sorted(set(expected_exports) - exported)
        if missing:
            raise RuntimeError(name + ' missing exports: ' + str(missing))

        dependencies = self.run(name + '-dependencies', [self.tool['readelf'], '-d', library])
        if 'sdl' in dependencies.lower():
            raise RuntimeError('Unexpected SDL dependency in ' + name)

    def run_tests(self, package):
        tests = self.out / 'tests'
        tests.mkdir()

        for name in POLICY_TESTS:
            exe = tests / Path(name).stem
            sources = [self.renderer / 'tests' / name]
            if name == 'x_error_ledger_tests.cpp':
                sources.append(self.renderer / 'x_error_ledger.cpp')
            self.run(exe.name + '-build', [*self.compile, '-Werror', '-DXTHREADS', *sources, '-o', exe])
            self.run(exe.name, [exe])

        bridge = tests / Path(BRIDGE_TEST).stem
        self.run(bridge.name + '-build', [
            *self.compile, '-Werror', self.renderer / 'tests' / BRIDGE_TEST,
            self.renderer / 'camera_bridge.cpp', self.renderer / 'camera_control.cpp',
            '-o', bridge, '-lX11', '-lXi', '-ldl'])
        self.run(bridge.name, [bridge, self.native / 'libSmf.Camera.so'])

        driver = tests / 'camera_abi_tests'
        self.run('camera_abi_tests-build', [*self.compile, '-Werror', self.snapshot / CAMERA_DRIVER, '-o', driver, '-ldl'])

        reference_output = self.out / 'reference-managed'
        self.run('camera-reference-build', [
            self.tool['dotnet'], 'build', self.snapshot / CAMERA_REFERENCE, *self.managed_options(),
            '-p:OutputPath=' + str(reference_output) + '/', '-p:AppendTargetFrameworkToOutputPath=false',
            '-p:AppendRuntimeIdentifierToOutputPath=false'])
        fixture = tests / 'reference.bin'
        reference_dll = reference_output / 'Smf.Camera.Reference.dll'
        package.ordinary(reference_dll)

        self.run('camera-reference', [self.tool['dotnet'], reference_dll, fixture])
        self.run('camera_abi_tests', [driver, self.native / 'libSmf.Camera.so', fixture, tests / 'camera-result'])

    def copy_notices(self):
        for name in NOTICES:
            shutil.copy2(self.camera / 'ThirdParty' / name, self.native / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New build directory; must not exist yet')
    parser.add_argument('--tests', action='store_true',
                        help='Also build and run the CPU policy tests and the camera ABI reference comparison')
    parser.add_argument('--plan', action='store_true', help='Report source readiness without running tools or writing')
    parser.add_argument('--force', action='store_true', help='Replace an existing output directory')
    args = parser.parse_args()

    package = load_package_module()
    missing = [relative(path) for path in required_files(args.tests) if not path.is_file()]
    sources = []
    inventory_error = None

    try:
        sources = package.source_files_for_native('linux-x64')
        for path in sources:
            package.ordinary(path)
    except (ValueError, OSError) as error:
        inventory_error = str(error)

    if inventory_error is None:
        # The snapshot is what gets compiled, so every shared header has to be in it.
        snapshotted = {relative(path) for path in sources}
        outside = [relative(path) for path in required_files(args.tests) if relative(path) not in snapshotted]
        if outside:
            inventory_error = 'Required files are not part of the source inventory: ' + str(outside)

    out = args.output.absolute()
    manifest = [record(path, ROOT) for path in sources]

    if args.plan:
        print(json.dumps({
            'platform': 'linux-x64',
            'output': str(out),
            'tests': args.tests,
            'ready': not missing and inventory_error is None,
            'missingCanonicalSources': missing,
            'inventoryError': inventory_error,
            'sources': manifest,
            'requiredTools': list(TOOLS),
            'linkLibraries': list(LINK_LIBRARIES),
            'gameExecuted': False,
            'graphicsExecuted': False,
        }, indent=2))
        return

    if missing or inventory_error:
        raise SystemExit('Canonical Linux sources are not ready: ' + str(missing or inventory_error))
    if platform.system() != 'Linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
        raise SystemExit('This build must run on Linux x64.')
    out = package.checked_output(args.output, args.force, sources)

    tool = {name: shutil.which(name) for name in TOOLS}
    absent = [name for name, path in tool.items() if not path]
    if absent:
        raise SystemExit('Missing required tools; install them separately: ' + str(absent))

    out = package.replace_output(args.output, args.force, sources)
    out.mkdir(parents=True, exist_ok=False)
    snapshot = out / 'source'
    logs = out / 'logs'
    native = out / 'Native/linux-x64'
    logs.mkdir()
    native.mkdir(parents=True)

    receipt = {
        'version': 1,
        'platform': 'linux-x64',
        'state': 'started',
        'startedUnix': time.time(),
        'sources': manifest,
        'steps': [],
        'gameExecuted': False,
        'graphicsExecuted': False,
        'cpuTestsRequested': args.tests,
        'msbuildWorkers': 1,
        'dotnetProcessorCount': 2,
        'requiredRendererExports': RENDERER_EXPORTS,
    }
    try:
        snapshot_sources(ROOT, sources, manifest, snapshot)

        receipt['tools'] = {}
        for name, path in tool.items():
            resolved = Path(path).resolve()
            receipt['tools'][name] = {'path': str(resolved), 'sha256': digest(resolved)}

        build = Build(out, snapshot, tool, receipt)
        build.run('compiler-version', [tool['c++'], '--version'])
        build.run('dotnet-info', [tool['dotnet'], '--info'])
        build.build_renderer()
        build.build_camera(package)
        build.check_library(package, 'libSmf.Renderer.so', RENDERER_EXPORTS)
        build.check_library(package, 'libSmf.Camera.so', CAMERA_EXPORTS)
        if args.tests:
            build.run_tests(package)
        build.copy_notices()

        if [record(path, ROOT) for path in package.source_files_for_native('linux-x64')] != manifest:
            raise RuntimeError('Canonical source inventory changed during build.')
        receipt['outputs'] = [record(native / name, out) for name in ('libSmf.Renderer.so', 'libSmf.Camera.so')]
        receipt['notices'] = [record(native / name, out) for name in NOTICES]
        receipt['state'] = 'built-cpu-tested' if args.tests else 'built-not-tested'
    except BaseException as error:
        receipt['state'] = 'failed'
        receipt['error'] = str(error)
        raise
    finally:
        receipt['finishedUnix'] = time.time()
        write_receipt(out, receipt)

    try:
        package.validate_native_renderer(out, 'linux-x64')
    except (ValueError, OSError, KeyError, TypeError) as error:
        receipt['state'] = 'failed'
        receipt['error'] = 'Final package validation: ' + str(error)
        write_receipt(out, receipt)
        raise

    print(json.dumps({
        'state': receipt['state'],
        'receipt': str(out / RECEIPT_NAME),
        'runtimeValidation': 'not established; the CPU tests load only the camera kernel and never open a game or graphics window',
    }, indent=2))


if __name__ == '__main__':
    main()
