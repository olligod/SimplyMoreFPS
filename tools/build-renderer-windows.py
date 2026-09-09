#!/usr/bin/env python3
"""Build the Windows x64 renderer and camera kernel, optionally running the CPU-only tests.

Requires Python 3.9+, the .NET 10 SDK with NativeAOT support and Visual Studio 2022
Build Tools (MSVC x64 and a Windows SDK). Nothing is installed and no game is started.
--plan only prints the source manifest.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = 'win-x64'
RENDERER_DIR = 'src/Smf.Renderer/windows'
VENDOR_DIR = 'src/Smf.Renderer/vendor/minhook'
CAMERA_DIR = 'src/Smf.Camera'

RENDERER_SOURCES = (
    'camera_bridge.cpp',
    'camera_control.cpp',
    'camera_math.cpp',
    'commit_completion.cpp',
    'present_observer.cpp',
    'session_bridge.cpp',
    'session_source.cpp',
    'session_worker.cpp',
)
MINHOOK_SOURCES = ('buffer.c', 'hook.c', 'trampoline.c', 'hde/hde64.c')
# Each test is windows/tests/<name>.cpp linked with the listed renderer sources and libraries.
RENDERER_TESTS = (
    ('session_policy_tests', (), ()),
    ('detour_chain_tests', (), ()),
    ('camera_wheel_tests', ('camera_control.cpp',), ('user32.lib',)),
    ('camera_bridge_trajectory_tests', ('camera_bridge.cpp', 'camera_control.cpp'), ('user32.lib',)),
    ('camera_status_tests', ('camera_bridge.cpp', 'camera_control.cpp'), ('user32.lib',)),
    ('full_map_cache_tests', ('camera_math.cpp',), ()),
    ('selection_overlay_tests', (), ()),
)
CAMERA_DRIVER = 'tests/native_driver.cpp'
CAMERA_REFERENCE = 'tests/Reference.csproj'
NOTICES = ('DOTNET-LICENSE.txt', 'DOTNET-THIRD-PARTY-NOTICES.txt')

SESSION_EXPORTS = ('start', 'command', 'content_fence', 'ack', 'status', 'pre_gui', 'frame', 'native_frame',
                   'cancel', 'render_event', 'poll_joined')
BRIDGE_EXPORTS = ('init', 'publish', 'revoke', 'desired', 'status', 'now')
CONTROL_EXPORTS = ('policy', 'impulse', 'status', 'wheel_status')
OBSERVER_EXPORTS = ('start', 'context', 'begin_source_frame', 'rendered', 'rendered_unity_target', 'status',
                    'read', 'read_v2', 'last_submission', 'chain_status')
RENDERER_EXPORTS = (tuple('smf_session_' + name for name in SESSION_EXPORTS)
                    + tuple('smf_camera_bridge_' + name for name in BRIDGE_EXPORTS)
                    + tuple('smf_camera_control_' + name for name in CONTROL_EXPORTS)
                    + ('smf_selection_publish',)
                    + tuple('smf_po_' + name for name in OBSERVER_EXPORTS))
CAMERA_EXPORTS = tuple('smf_camera_' + name for name in ('create', 'adopt', 'configure', 'step', 'release'))

TOOLS = ('cl.exe', 'link.exe', 'dumpbin.exe', 'dotnet.exe')
RENDERER_LIBRARIES = ('d3d11.lib', 'dxgi.lib', 'dcomp.lib', 'ole32.lib', 'user32.lib')
RECEIPT_NAME = 'build-receipt.json'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def record(path, root):
    return {'path': path.relative_to(root).as_posix(), 'sha256': digest(path), 'bytes': path.stat().st_size}


def load_package_module():
    """tools/package-release.py owns the source inventory and the final validation."""
    spec = importlib.util.spec_from_file_location('smf_windows_package', ROOT / 'tools/package-release.py')
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    try:
        sys.dont_write_bytecode = True
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module


def find_vcvars(vcvars):
    """vcvars64.bat as given, or the one vswhere finds for the latest x64 MSVC tools."""
    if vcvars is None:
        vswhere = Path(os.environ.get('ProgramFiles(x86)', '')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        if not vswhere.is_file():
            raise RuntimeError('Install Visual Studio Build Tools or pass --vcvars')

        found = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                                         'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                         '-property', 'installationPath'], text=True).strip()
        if not found:
            raise RuntimeError('No x64 MSVC Build Tools installation found')
        vcvars = Path(found) / 'VC/Auxiliary/Build/vcvars64.bat'

    vcvars = vcvars.resolve(strict=True)
    if any(c in str(vcvars) for c in '"\r\n%'):
        raise ValueError('Unsupported characters in the vcvars path')
    return vcvars


def tool_environment(vcvars, out):
    """The environment vcvars64.bat leaves behind, and the tools found on its PATH."""
    script = out / 'toolchain-env.cmd'
    script.write_text('@echo off\ncall "' + str(vcvars) + '" >nul\nif errorlevel 1 exit /b %errorlevel%\nset\n',
                      encoding='utf-8')

    values = subprocess.check_output(['cmd.exe', '/d', '/c', str(script)], text=True)
    env = {key.upper(): value for key, value in os.environ.items()}
    for line in values.splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            env[key.upper()] = value
    env['DOTNET_PROCESSOR_COUNT'] = '2'

    tools = {name: shutil.which(name, path=env.get('PATH')) for name in TOOLS}
    missing = [name for name, path in tools.items() if not path]
    if missing:
        raise RuntimeError('Missing required tools: ' + ', '.join(missing))
    return env, {name: Path(path) for name, path in tools.items()}


class Build:
    def __init__(self, out, snapshot, env, tool, receipt):
        self.out = out
        self.logs = out / 'logs'
        self.snapshot = snapshot
        self.env = env
        self.tool = tool
        self.receipt = receipt
        self.renderer = snapshot / RENDERER_DIR
        self.vendor = snapshot / VENDOR_DIR
        self.camera = snapshot / CAMERA_DIR
        self.native = out / 'Native' / RUNTIME
        self.compile = [tool['cl.exe'], '/nologo', '/std:c++17', '/O2', '/W4', '/WX', '/MT', '/EHsc']
        self.minhook_objects = [out / 'obj/vendor' / (Path(name).stem + '.obj') for name in MINHOOK_SOURCES]

    def run(self, label, command):
        """Run one step with its output captured to a log; the receipt keeps the exit code."""
        command = [str(part) for part in command]
        log = self.logs / (label + '.log')
        start = time.time()

        with log.open('w', encoding='utf-8') as stream:
            result = subprocess.run(command, cwd=self.out, env=self.env, stdout=stream, stderr=subprocess.STDOUT)

        self.receipt['steps'].append({
            'label': label,
            'command': command,
            'exitCode': result.returncode,
            'elapsedSeconds': time.time() - start,
            'log': log.relative_to(self.out).as_posix(),
            'logSha256': digest(log),
        })

        print(label + ': ' + str(result.returncode), flush=True)
        if result.returncode:
            raise RuntimeError(label + ' failed; see ' + str(log))
        return log.read_text(encoding='utf-8', errors='replace')

    def managed_options(self, label):
        intermediate = str(self.out / 'obj' / label) + '/'
        return ['-c', 'Release', '-m:1', '-p:UseSharedCompilation=false', '-p:BuildInParallel=false',
                '-p:BaseIntermediateOutputPath=' + intermediate, '-p:MSBuildProjectExtensionsPath=' + intermediate,
                '-p:OutputPath=' + str(self.out / 'bin' / label) + '/',
                '-p:PathMap=' + str(self.snapshot) + '=/_/source']

    def build_minhook(self):
        obj = self.out / 'obj/vendor'
        obj.mkdir(parents=True)
        sources = [self.vendor / 'src' / name for name in MINHOOK_SOURCES]
        self.run('minhook', [self.tool['cl.exe'], '/nologo', '/O2', '/W4', '/MT', '/TC', '/c',
                             '/Fo' + str(obj) + '\\', *sources])

    def build_renderer(self):
        obj = self.out / 'obj/renderer'
        obj.mkdir(parents=True)
        sources = [self.renderer / name for name in RENDERER_SOURCES]
        self.run('renderer', [*self.compile, '/DUNICODE', '/D_UNICODE', '/LD', '/Fo' + str(obj) + '\\',
                              '/Fe' + str(self.native / 'Smf.Renderer.dll'), *sources, *self.minhook_objects,
                              '/link', '/INCREMENTAL:NO', '/Brepro', *RENDERER_LIBRARIES])

    def build_camera(self):
        self.run('camera', [self.tool['dotnet.exe'], 'publish', self.camera / 'Smf.Camera.csproj', '-r', RUNTIME,
                            *self.managed_options('camera'), '-p:PublishDir=' + str(self.native) + '/'])

    def check_library(self, name, expected_exports):
        library = self.native / name
        exports = self.run(library.stem + '-exports', [self.tool['dumpbin.exe'], '/nologo', '/exports', library])
        missing = sorted(set(expected_exports) - set(exports.split()))
        if missing:
            raise RuntimeError(name + ' missing exports: ' + str(missing))

        dependencies = self.run(library.stem + '-dependencies',
                                [self.tool['dumpbin.exe'], '/nologo', '/dependents', library]).upper()
        if 'VCRUNTIME' in dependencies or 'MSVCP' in dependencies:
            raise RuntimeError('Unexpected VC redistributable dependency in ' + name)

    def build_test(self, name, sources, libraries):
        tests = self.out / 'tests'
        obj = tests / (name + '-obj')
        obj.mkdir()
        exe = tests / (name + '.exe')

        self.run(name + '-build', [*self.compile, '/Fo' + str(obj) + '\\', '/Fe' + str(exe), *sources,
                                   '/link', '/INCREMENTAL:NO', *libraries])
        return exe

    def run_tests(self):
        tests = self.out / 'tests'
        tests.mkdir()
        camera_dll = self.native / 'Smf.Camera.dll'

        for name, linked, libraries in RENDERER_TESTS:
            sources = [self.renderer / 'tests' / (name + '.cpp')]
            sources += [self.renderer / file for file in linked]
            if name == 'detour_chain_tests':
                sources += self.minhook_objects
            exe = self.build_test(name, sources, libraries)
            # The trajectory test drives the real bridge against the built kernel.
            arguments = [camera_dll] if name == 'camera_bridge_trajectory_tests' else []
            self.run(name, [exe, *arguments])

        driver = self.build_test('camera_abi_tests', [self.camera / CAMERA_DRIVER], ())
        self.run('camera-reference-build', [self.tool['dotnet.exe'], 'build', self.camera / CAMERA_REFERENCE,
                                            *self.managed_options('reference')])

        fixture = tests / 'reference.bin'
        self.run('camera-reference', [self.tool['dotnet.exe'], self.out / 'bin/reference/Smf.Camera.Reference.dll', fixture])
        self.run('camera_abi_tests', [driver, camera_dll, fixture, tests / 'camera-result'])

    def stage_notices(self):
        shutil.copy2(self.vendor / 'LICENSE.txt', self.native / 'MinHook-LICENSE.txt')
        for notice in NOTICES:
            shutil.copy2(self.camera / 'ThirdParty' / notice, self.native / notice)

    def output_record(self, name):
        path = self.native / name
        return {'path': path.relative_to(self.out).as_posix(), 'sha256': digest(path)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True,
                        help='New build directory; gets Native/win-x64 and a source snapshot')
    parser.add_argument('--vcvars', type=Path, help='vcvars64.bat to use; found with vswhere when omitted')
    parser.add_argument('--tests', action='store_true', help='Also compile and run the CPU-only tests')
    parser.add_argument('--plan', action='store_true', help='Print the source manifest and stop')
    parser.add_argument('--force', action='store_true', help='Replace an existing output directory')
    args = parser.parse_args()

    package = load_package_module()
    sources = package.source_files_for_native(RUNTIME)
    manifest = [record(path, ROOT) for path in sources]
    out = args.output.absolute()

    if args.plan:
        print(json.dumps({'output': str(out), 'tests': args.tests, 'sources': manifest}, indent=2))
        return

    if os.name != 'nt':
        raise SystemExit('This renderer build targets Windows x64 only.')

    out = package.replace_output(args.output, args.force, sources)
    out.mkdir(parents=True, exist_ok=False)
    snapshot = out / 'source'
    for path in sources:
        target = snapshot / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)

    (out / 'Native' / RUNTIME).mkdir(parents=True)
    (out / 'logs').mkdir()

    receipt = {
        'version': 1,
        'state': 'started',
        'startedUnix': time.time(),
        'sources': manifest,
        'platform': RUNTIME,
        'gameExecuted': False,
        'graphicsExecuted': False,
        'cpuTestsRequested': args.tests,
        'msbuildWorkers': 1,
        'dotnetProcessorCount': 2,
        'steps': [],
    }
    try:
        vcvars = find_vcvars(args.vcvars)
        env, tool = tool_environment(vcvars, out)
        receipt['tools'] = {name: {'path': str(path), 'sha256': digest(path)} for name, path in tool.items()}
        receipt['compilerEnvironment'] = {'path': str(vcvars), 'sha256': digest(vcvars)}

        build = Build(out, snapshot, env, tool, receipt)
        build.run('dotnet-info', [tool['dotnet.exe'], '--info'])
        build.build_minhook()
        build.build_renderer()
        build.build_camera()
        build.check_library('Smf.Renderer.dll', RENDERER_EXPORTS)
        build.check_library('Smf.Camera.dll', CAMERA_EXPORTS)
        receipt['requiredRendererExports'] = list(RENDERER_EXPORTS)
        if args.tests:
            build.run_tests()

        # Only the two DLLs and the notices get packaged; objects, symbols, tests and logs stay here.
        build.stage_notices()
        receipt['outputs'] = [build.output_record(name) for name in ('Smf.Renderer.dll', 'Smf.Camera.dll')]
        receipt['notices'] = [build.output_record(name) for name in ('MinHook-LICENSE.txt', *NOTICES)]
        receipt['state'] = 'built-cpu-tested' if args.tests else 'built-not-tested'
    except BaseException as error:
        receipt['state'] = 'failed'
        receipt['error'] = str(error)
        raise
    finally:
        receipt['finishedUnix'] = time.time()
        (out / RECEIPT_NAME).write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')

    print(json.dumps({'state': receipt['state'], 'outputs': receipt['outputs']}, indent=2))


if __name__ == '__main__':
    main()
