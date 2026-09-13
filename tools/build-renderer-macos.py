#!/usr/bin/env python3
"""Build the universal macOS renderer bundle and camera kernel, optionally running the CPU-only tests.

Requires the Xcode command line tools and .NET 10 NativeAOT on a macOS host. Nothing is
installed and no game is started. The tests run on the host architecture only and never
load the renderer. --plan only reports readiness.
"""
import argparse
import json
import os
import platform
import plistlib
import shutil
import sys
import time
from pathlib import Path

# CI also imports these scripts by path, without tools/ on its module search path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_build import BuildLog, RECEIPT_NAME, load_package, snapshot_sources, write_receipt
from native_build import file_digest as digest, file_record as record

ROOT = Path(__file__).resolve().parents[1]
RENDERER_DIR = ROOT / 'src/Smf.Renderer/mac'

RENDERER_SOURCES = (
    'mac_session.mm',
    'metal_target.mm',
    'target_probe.mm',
    'copy_probe.mm',
    'metal_source.mm',
    'metal_worker.mm',
    'scene_compositor.mm',
    'window_owner.mm',
    'drawable_observer.mm',
    'original_present.mm',
    'present_observer.mm',
    'mac_input.mm',
    'mac_camera.cpp',
)
POLICY_TESTS = (
    'ownership_tests.cpp',
    'activation_tests.cpp',
    'source_pass_policy_tests.cpp',
    'target_policy_tests.cpp',
    'present_model_tests.cpp',
    'projection_tests.cpp',
    'source_rejection_tests.cpp',
    'present_recovery_tests.cpp',
    'present_model_bounds_tests.cpp',
    'copy_disposition_tests.cpp',
    'geometry_diagnostics_tests.cpp',
)
# The camera ABI check lives with the kernel, outside mac/.
CAMERA_DRIVER = 'src/Smf.Camera/tests/native_driver.cpp'
CAMERA_REFERENCE = 'src/Smf.Camera/tests/Reference.csproj'
# The Unity plugin headers carry their own notices; they are appended to the third party file.
UNITY_DIR = 'src/Smf.Renderer/mac/unity'
UNITY_LICENSE = 'LICENSE'
UNITY_COMPANION_LICENSE = 'UNITY-COMPANION-LICENSE.txt'
NOTICES = ('DOTNET-LICENSE.txt', 'DOTNET-THIRD-PARTY-NOTICES.txt')

ARCHITECTURES = {'x86_64': 'osx-x64', 'arm64': 'osx-arm64'}
FRAMEWORKS = ('AppKit', 'QuartzCore', 'Metal', 'Carbon', 'CoreGraphics')
TOOLS = ('xcrun', 'lipo', 'nm', 'codesign', 'otool', 'dotnet')

MAC_EXPORTS = ('original_base_enable', 'find_original_window', 'source_base', 'capabilities',
               'source_target_enable', 'source_target', 'native_target', 'geometry_failure', 'presentation')
SESSION_EXPORTS = ('clock_now', 'clock_frequency', 'start', 'command', 'content_fence', 'ack', 'status',
                   'pre_gui', 'frame', 'native_frame', 'cancel', 'render_event', 'poll_joined', 'quit')
BRIDGE_EXPORTS = ('init', 'publish', 'revoke', 'desired', 'status', 'now')
CONTROL_EXPORTS = ('policy', 'impulse', 'status', 'wheel_status')
RENDERER_EXPORTS = (('UnityPluginLoad', 'UnityPluginUnload')
                    + tuple('smf_mac_' + name for name in MAC_EXPORTS)
                    + tuple('smf_session_' + name for name in SESSION_EXPORTS)
                    + tuple('smf_camera_bridge_' + name for name in BRIDGE_EXPORTS)
                    + tuple('smf_camera_control_' + name for name in CONTROL_EXPORTS)
                    + ('smf_selection_publish',))
CAMERA_EXPORTS = tuple('smf_camera_' + name for name in ('create', 'adopt', 'configure', 'step', 'release'))

INFO_PLIST = {
    'CFBundleIdentifier': 'org.simplymorefps.renderer',
    'CFBundleName': 'Smf.Renderer',
    'CFBundleExecutable': 'Smf.Renderer',
    'CFBundlePackageType': 'BNDL',
    'CFBundleVersion': '1',
    'LSMinimumSystemVersion': '11.0',
}


def load_package_module():
    return load_package(ROOT, 'smf_mac_package')


def required_files(with_tests):
    files = [RENDERER_DIR / name for name in RENDERER_SOURCES]
    files += [ROOT / 'src/Smf.Camera/camera_kernel_arm64.S']
    files += [ROOT / UNITY_DIR / name for name in (UNITY_LICENSE, UNITY_COMPANION_LICENSE)]
    if with_tests:
        files += [RENDERER_DIR / 'tests' / name for name in POLICY_TESTS]
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
        self.env = dict(os.environ, DOTNET_PROCESSOR_COUNT='2', MSBUILDDISABLENODEREUSE='1')
        self.log_runner = BuildLog(out, self.env, receipt, encoding=None)
        self.renderer = snapshot / 'src/Smf.Renderer/mac'
        self.camera = snapshot / 'src/Smf.Camera'
        self.native = out / 'Native/osx-universal'
        self.bundle = self.native / 'Smf.Renderer.bundle'
        self.binary = self.bundle / 'Contents/MacOS/Smf.Renderer'
        self.kernel = self.native / 'libSmf.Camera.dylib'
        self.compile = []

    def run(self, label, command):
        return self.log_runner.run(label, command)

    def find_compiler(self):
        compiler = Path(self.run('find-clang', [self.tool['xcrun'], '--find', 'clang++']).strip())
        sdk = Path(self.run('find-sdk', [self.tool['xcrun'], '--sdk', 'macosx', '--show-sdk-path']).strip())

        self.receipt['compiler'] = {'path': str(compiler), 'sha256': digest(compiler), 'sdk': str(sdk)}
        self.compile = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror=return-type',
                        '-Wno-deprecated-declarations', '-mmacosx-version-min=11.0', '-isysroot', sdk]
        self.run('compiler-version', [compiler, '--version'])

    def managed_options(self):
        return ['-c', 'Release', '-m:1', '--disable-build-servers', '-p:UseSharedCompilation=false',
                '-p:BuildInParallel=false', '--artifacts-path', self.out / 'managed']

    def build_renderer_slice(self, arch):
        objects = []
        obj = self.out / 'obj' / arch
        obj.mkdir(parents=True)

        for name in RENDERER_SOURCES:
            source = self.renderer / name
            target = obj / (source.stem + '.o')
            if source.suffix == '.mm':
                language = ['-x', 'objective-c++', '-fobjc-arc']
            else:
                language = ['-x', 'c++']

            self.run('compile-' + arch + '-' + source.stem,
                     [*self.compile, '-arch', arch, *language, '-fvisibility=hidden', '-c', source, '-o', target])
            objects.append(target)

        # Link the objects as they are; no input language or ARC is forced onto the C++ ones.
        frameworks = [part for name in FRAMEWORKS for part in ('-framework', name)]
        thin = self.out / ('renderer-' + arch)
        self.run('renderer-' + arch, [*self.compile, '-arch', arch, *objects, *frameworks, '-lobjc',
                                      '-bundle', '-Wl,-undefined,error', '-o', thin])

        return thin

    def build_camera_slice(self, package, arch, rid):
        dest = self.out / 'camera' / rid
        options = []
        if arch == 'arm64':
            obj = self.out / 'obj' / arch / 'camera_kernel_arm64.o'
            obj.parent.mkdir(parents=True, exist_ok=True)
            self.run('camera-abi-shim-' + arch,
                     [self.compile[0], '-arch', arch, '-isysroot', self.receipt['compiler']['sdk'],
                      '-mmacosx-version-min=11.0', '-x', 'assembler-with-cpp', '-c',
                      self.camera / 'camera_kernel_arm64.S', '-o', obj])
            options.append('-p:SmfCameraAbiObject=' + str(obj))

        self.run('camera-' + arch, [self.tool['dotnet'], 'publish', self.camera / 'Smf.Camera.csproj', '-r', rid,
                                    *self.managed_options(), '-p:IlcSingleThreaded=true', *options, '-o', dest])

        kernel = dest / 'Smf.Camera.dylib'
        package.ordinary(kernel)
        return kernel

    def check_slice(self, label, arch, path, expected_exports):
        symbols = self.run(label + '-exports-' + arch, [self.tool['nm'], '-arch', arch, '-gU', path]).split()
        missing = sorted(name for name in expected_exports if '_' + name not in symbols)
        if missing:
            raise RuntimeError('Missing ' + label + ' exports in ' + arch + ': ' + str(missing))

        dependencies = self.run(label + '-dependencies-' + arch, [self.tool['otool'], '-L', path])
        if 'sdl' in dependencies.lower():
            raise RuntimeError('Unexpected SDL dependency in ' + label + ' ' + arch)

    def build_universal(self, package):
        slices = []
        kernels = []

        for arch, rid in ARCHITECTURES.items():
            thin = self.build_renderer_slice(arch)
            kernel = self.build_camera_slice(package, arch, rid)
            self.check_slice('renderer', arch, thin, RENDERER_EXPORTS)
            self.check_slice('camera', arch, kernel, CAMERA_EXPORTS)
            slices.append(thin)
            kernels.append(kernel)

        self.binary.parent.mkdir(parents=True)
        self.run('renderer-universal', [self.tool['lipo'], '-create', *slices, '-output', self.binary])
        self.run('camera-universal', [self.tool['lipo'], '-create', *kernels, '-output', self.kernel])

        for label, path in (('renderer', self.binary), ('camera', self.kernel)):
            archs = set(self.run(label + '-architectures', [self.tool['lipo'], '-archs', path]).split())
            if archs != set(ARCHITECTURES):
                raise RuntimeError('Both universal slices are required in ' + label)

        (self.bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps(INFO_PLIST))
        for label, path in (('renderer', self.bundle), ('camera', self.kernel)):
            self.run(label + '-sign', [self.tool['codesign'], '--force', '--sign', '-', path])
            self.run(label + '-verify', [self.tool['codesign'], '--verify', '--strict', path])

    def run_tests(self, host):
        tests = self.out / 'tests'
        tests.mkdir()

        for name in POLICY_TESTS:
            exe = tests / Path(name).stem
            self.run(exe.name + '-build', [*self.compile, '-arch', host, self.renderer / 'tests' / name, '-o', exe])
            self.run(exe.name, [exe])

        driver = tests / 'CameraAbiTests'
        self.run('camera-driver-build', [*self.compile, '-arch', host, self.snapshot / CAMERA_DRIVER, '-o', driver])
        reference_output = self.out / 'reference-managed'
        self.run('camera-reference-build', [
            self.tool['dotnet'], 'build', self.snapshot / CAMERA_REFERENCE, *self.managed_options(),
            '-p:OutputPath=' + str(reference_output) + '/', '-p:AppendTargetFrameworkToOutputPath=false',
            '-p:AppendRuntimeIdentifierToOutputPath=false'])

        fixture = tests / 'reference.bin'
        self.run('camera-reference', [self.tool['dotnet'], reference_output / 'Smf.Camera.Reference.dll', fixture])
        self.run('camera-abi', [driver, self.kernel, fixture, tests / 'camera-result'])

    def copy_notices(self):
        third_party = self.camera / 'ThirdParty'
        shutil.copy2(third_party / 'DOTNET-LICENSE.txt', self.native / 'DOTNET-LICENSE.txt')

        unity = self.snapshot / UNITY_DIR
        notices = (third_party / 'DOTNET-THIRD-PARTY-NOTICES.txt').read_bytes()
        notices += b'\n\nUnity native plugin header notices (preserved from source):\n'
        notices += (unity / UNITY_LICENSE).read_bytes()
        for header in sorted(unity.glob('*.h')):
            # The notice is the header text before its include guard.
            notices += b'\n' + header.name.encode() + b'\n' + header.read_bytes().split(b'#pragma once', 1)[0]

        notices += b'\n\nUnity Companion License (full upstream notice):\n'
        notices += (unity / UNITY_COMPANION_LICENSE).read_bytes()
        (self.native / 'DOTNET-THIRD-PARTY-NOTICES.txt').write_bytes(notices)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New build directory; must not exist yet')
    parser.add_argument('--tests', action='store_true',
                        help='Also build and run the CPU policy tests and the camera ABI comparison on this host architecture')
    parser.add_argument('--plan', action='store_true', help='Report source readiness without running tools or writing')
    parser.add_argument('--force', action='store_true', help='Replace an existing output directory')
    args = parser.parse_args()

    package = load_package_module()
    missing = [relative(path) for path in required_files(args.tests) if not path.is_file()]
    sources = []
    inventory_error = None

    try:
        sources = package.source_files_for_native('osx-universal')
        for path in sources:
            package.ordinary(path)
    except (ValueError, OSError) as error:
        inventory_error = str(error)

    if inventory_error is None:
        # The snapshot is what gets compiled, so every required file has to be in it.
        snapshotted = {relative(path) for path in sources}
        outside = [relative(path) for path in required_files(args.tests) if relative(path) not in snapshotted]
        if outside:
            inventory_error = 'Required files are not part of the source inventory: ' + str(outside)

    out = args.output.absolute()
    manifest = [record(path, ROOT) for path in sources]

    if args.plan:
        print(json.dumps({
            'ready': not missing and inventory_error is None,
            'missing': missing,
            'inventoryError': inventory_error,
            'sources': manifest,
            'architectures': list(ARCHITECTURES),
            'tests': 'host CPU/ABI only' if args.tests else 'none',
        }, indent=2))
        return

    if missing or inventory_error:
        raise SystemExit('Canonical macOS sources are not ready: ' + str(missing or inventory_error))
    host = platform.machine()
    if platform.system() != 'Darwin' or host not in ARCHITECTURES:
        raise SystemExit('Build on a supported macOS host.')
    out = package.checked_output(args.output, args.force, sources)

    tool = {name: shutil.which(name) for name in TOOLS}
    absent = [name for name, path in tool.items() if not path]
    if absent:
        raise SystemExit('Missing required tools; install them separately: ' + str(absent))

    out = package.replace_output(args.output, args.force, sources)
    out.mkdir(parents=True, exist_ok=False)
    snapshot = out / 'source'
    logs = out / 'logs'
    native = out / 'Native/osx-universal'
    logs.mkdir()
    native.mkdir(parents=True)

    receipt = {
        'version': 1,
        'platform': 'osx-universal',
        'state': 'started',
        'startedUnix': time.time(),
        'sources': manifest,
        'steps': [],
        'gameExecuted': False,
        'graphicsExecuted': False,
        'rendererLoaded': False,
        'cpuTestsRequested': args.tests,
        'cpuTestArchitecture': host if args.tests else None,
        'msbuildWorkers': 1,
        'dotnetProcessorCount': 2,
    }
    try:
        snapshot_sources(ROOT, sources, manifest, snapshot)

        receipt['tools'] = {}
        for name, path in tool.items():
            resolved = Path(path).resolve()
            receipt['tools'][name] = {'path': str(resolved), 'sha256': digest(resolved)}

        build = Build(out, snapshot, tool, receipt)
        build.find_compiler()
        build.run('dotnet-info', [tool['dotnet'], '--info'])
        build.build_universal(package)
        if args.tests:
            build.run_tests(host)
        build.copy_notices()

        if [record(path, ROOT) for path in package.source_files_for_native('osx-universal')] != manifest:
            raise RuntimeError('Canonical sources changed during build.')
        produced = package.NATIVE_FILES['osx-universal']
        receipt['outputs'] = [record(native / name, out) for name in produced if not name.endswith('.txt')]
        receipt['notices'] = [record(native / name, out) for name in produced if name.endswith('.txt')]
        package.verify_mac_bundle(native)
        receipt['state'] = 'built-cpu-tested' if args.tests else 'built-not-tested'
    except BaseException as error:
        receipt['state'] = 'failed'
        receipt['error'] = str(error)
        raise
    finally:
        receipt['finishedUnix'] = time.time()
        write_receipt(out, receipt)

    try:
        package.validate_native_renderer(out, 'osx-universal')
    except (ValueError, OSError, KeyError, TypeError) as error:
        receipt['state'] = 'failed'
        receipt['error'] = 'Final package validation: ' + str(error)
        write_receipt(out, receipt)
        raise

    print(json.dumps({
        'state': receipt['state'],
        'testsArchitecture': receipt['cpuTestArchitecture'],
        'runtimeValidation': 'not established',
    }, indent=2))


if __name__ == '__main__':
    main()
