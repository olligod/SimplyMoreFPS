#!/usr/bin/env python3
"""Check packaged native loading with and without quarantine, without changing policy."""
import argparse
import hashlib
import json
import platform
import shutil
import subprocess
import time
import uuid
from pathlib import Path


PROBE = r'''
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        fprintf(stderr, "%s\n", dlerror());
        return 3;
    }
    for (int i = 2; i < argc; ++i) {
        dlerror();
        void *symbol = dlsym(module, argv[i]);
        const char *error = dlerror();
        if (error || !symbol) {
            fprintf(stderr, "%s: %s\n", argv[i], error ? error : "null export");
            return 4;
        }
    }
    // Do not call Unity exports or unload a NativeAOT library.
    puts("Loaded and resolved exports; no exports invoked.");
    fflush(stdout);
    _exit(0);
}
'''


def run(command):
    command = [str(value) for value in command]
    try:
        result = subprocess.run(command, capture_output=True, timeout=30)
        return {'command': command, 'exit': result.returncode,
                'stdout': result.stdout.decode('utf-8', errors='replace')[-16000:],
                'stderr': result.stderr.decode('utf-8', errors='replace')[-16000:]}
    except subprocess.TimeoutExpired as error:
        return {'command': command, 'exit': None, 'timeout': True,
                'stdout': (error.stdout or b'').decode('utf-8', errors='replace')[-16000:],
                'stderr': (error.stderr or b'').decode('utf-8', errors='replace')[-16000:]}


def attributes(path):
    return run(['xattr', '-l', path])


def inspect(probe, directory):
    bundle = directory / 'Smf.Renderer.bundle'
    files = [('renderer', bundle / 'Contents/MacOS/Smf.Renderer', bundle,
              ['UnityPluginLoad', 'UnityPluginUnload', 'smf_session_start']),
             ('camera', directory / 'libSmf.Camera.dylib', directory / 'libSmf.Camera.dylib',
              ['smf_camera_create', 'smf_camera_adopt', 'smf_camera_configure',
               'smf_camera_step', 'smf_camera_release'])]
    results = []
    for name, binary, signed, exports in files:
        results.append({'name': name, 'binary': str(binary),
                        'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                        'binaryAttributes': attributes(binary),
                        'signedObjectAttributes': attributes(signed),
                        'signature': run(['codesign', '-dv', '--verbose=4', signed]),
                        'verification': run(['codesign', '--verify', '--strict', signed]),
                        'load': run([probe, binary, *exports])})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', required=True, type=Path,
                        help='Extracted Native/osx-universal directory')
    parser.add_argument('--output', required=True, type=Path, help='New diagnostic directory')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('This check requires a macOS host.')
    native = args.native.resolve(strict=True)
    output = args.output.resolve()
    if output == native or native in output.parents:
        parser.error('Keep diagnostics outside the native input directory.')
    output.mkdir(parents=True, exist_ok=False)
    report = {'host': platform.machine(), 'macOS': platform.mac_ver()[0],
              'scope': 'Native CLI loading, not a Unity, Steam Workshop or notarization acceptance test.',
              'gatekeeper': run(['spctl', '--status'])}
    try:
        source = output / 'load-probe.c'
        source.write_text(PROBE, encoding='ascii')
        probe = output / 'load-probe'
        report['compile'] = run(['xcrun', 'clang', source, '-o', probe])
        if report['compile']['exit'] != 0:
            raise RuntimeError('Load probe did not compile.')
        report['ordinary'] = inspect(probe, native)

        # Quarantine only a disposable copy; preserve the distribution files.
        copied = output / 'quarantined'
        copied.mkdir()
        shutil.copytree(native / 'Smf.Renderer.bundle', copied / 'Smf.Renderer.bundle')
        shutil.copy2(native / 'libSmf.Camera.dylib', copied / 'libSmf.Camera.dylib')
        quarantine = ('0081;%08x;SMF CI loading diagnostic;%s' %
                      (int(time.time()), uuid.uuid4()))
        report['quarantineApplied'] = run(['xattr', '-r', '-w', 'com.apple.quarantine', quarantine, copied])
        if report['quarantineApplied']['exit'] != 0:
            raise RuntimeError('Could not quarantine the diagnostic copy.')
        report['quarantined'] = inspect(probe, copied)
        report['passed'] = all(item[key]['exit'] == 0
                               for variant in ('ordinary', 'quarantined')
                               for item in report[variant] for key in ('verification', 'load'))
    except Exception as error:
        report['error'] = str(error)
        report['passed'] = False
    finally:
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'passed': report['passed'], 'report': str(output / 'result.json')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
