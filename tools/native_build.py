"""File snapshots and command receipts shared by the native build scripts."""
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import time

RECEIPT_NAME = 'build-receipt.json'


def file_digest(path, *, uppercase=False):
    value = hashlib.sha256(path.read_bytes()).hexdigest()
    return value.upper() if uppercase else value


def file_record(path, root, *, uppercase=False):
    return {
        'path': path.relative_to(root).as_posix(),
        'sha256': file_digest(path, uppercase=uppercase),
        'bytes': path.stat().st_size,
    }


def load_package(root, module_name):
    spec = importlib.util.spec_from_file_location(module_name, root / 'tools/package-release.py')
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    try:
        sys.dont_write_bytecode = True
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module


def snapshot_sources(root, sources, manifest, snapshot, *, uppercase=False):
    for source in sources:
        target = snapshot / source.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

    copied = [file_record(snapshot / source.relative_to(root), snapshot, uppercase=uppercase)
              for source in sources]
    if copied != manifest:
        raise RuntimeError('Sources changed while snapshotting; discard this failed build and retry fresh.')


def write_receipt(out, receipt):
    (out / RECEIPT_NAME).write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')


class BuildLog:
    def __init__(self, out, env, receipt, *, uppercase_hashes=False, encoding='utf-8'):
        self.out = out
        self.env = env
        self.receipt = receipt
        self.uppercase_hashes = uppercase_hashes
        self.encoding = encoding

    def run(self, label, command):
        command = [str(part) for part in command]
        log = self.out / 'logs' / (label + '.log')
        start = time.time()

        with log.open('w', encoding=self.encoding) as stream:
            result = subprocess.run(command, cwd=self.out, env=self.env, stdout=stream, stderr=subprocess.STDOUT)

        self.receipt['steps'].append({
            'label': label,
            'command': command,
            'exitCode': result.returncode,
            'elapsedSeconds': time.time() - start,
            'log': log.relative_to(self.out).as_posix(),
            'logSha256': file_digest(log, uppercase=self.uppercase_hashes),
        })

        print(label + ': ' + str(result.returncode), flush=True)
        if result.returncode:
            raise RuntimeError(label + ' failed; see ' + str(log))
        return log.read_text(encoding=self.encoding, errors='replace')
