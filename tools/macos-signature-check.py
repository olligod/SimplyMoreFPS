#!/usr/bin/env python3
"""Check that a Mach-O code signature matches the file's contents. Signer trust is not checked."""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

# Magic numbers map to the byte order of the header behind them.
THIN_MAGIC = {b'\xcf\xfa\xed\xfe': '<', b'\xfe\xed\xfa\xcf': '>'}
FAT_MAGIC = {
    b'\xca\xfe\xba\xbe': ('>', False),
    b'\xbe\xba\xfe\xca': ('<', False),
    b'\xca\xfe\xba\xbf': ('>', True),
    b'\xbf\xba\xfe\xca': ('<', True),
}
# CodeDirectory hash type: (hashlib name, stored digest length)
HASH_TYPES = {1: ('sha1', 20), 2: ('sha256', 32), 3: ('sha256', 20), 4: ('sha384', 48)}
LC_CODE_SIGNATURE = 0x1D
LC_VERSION_MIN_MACOSX = 0x24
LC_BUILD_VERSION = 0x32
CSMAGIC_CODEDIRECTORY = 0xFADE0C02
CSMAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0
CS_ADHOC = 0x2


def require(condition, message):
    if not condition:
        raise ValueError(message)


def part(data, offset, length):
    require(offset >= 0 and length >= 0 and offset + length <= len(data), 'Structure runs past the end of the file')
    return data[offset:offset + length]


def format_version(value):
    return '%d.%d.%d' % (value >> 16, value >> 8 & 255, value & 255)


def check_code_directory(macho, blob, slot, signature_offset, embedded, external):
    """Recompute every code page hash and special slot hash of one CodeDirectory."""
    require(len(blob) >= 44, 'Truncated CodeDirectory')
    (magic, length, version, flags, hash_offset, ident_offset,
     special_slots, code_slots, code_limit) = struct.unpack_from('>9I', blob)
    require(magic == CSMAGIC_CODEDIRECTORY and length == len(blob), 'Invalid CodeDirectory')
    hash_size, hash_type, _, page_shift = struct.unpack_from('4B', blob, 36)
    require(hash_type in HASH_TYPES and hash_size == HASH_TYPES[hash_type][1], 'Unsupported CodeDirectory digest')
    require(page_shift <= 30, 'Invalid signature page size')

    if version >= 0x20100:
        scatter = struct.unpack_from('>I', blob, 44)[0] if len(blob) >= 48 else None
        require(scatter == 0, 'Scatter CodeDirectories are not supported')
    if version >= 0x20300:
        require(len(blob) >= 64, 'Truncated 64-bit CodeDirectory')
        if code_limit == 0xFFFFFFFF:
            code_limit = struct.unpack_from('>Q', blob, 56)[0]

    require(0 < code_limit <= signature_offset, 'Signed code range is empty or overlaps the signature')
    page_size = (1 << page_shift) if page_shift else code_limit
    require(code_slots == (code_limit + page_size - 1) // page_size, 'Code slot count does not cover the signed range')
    part(blob, hash_offset - special_slots * hash_size, (special_slots + code_slots) * hash_size)
    require(44 <= ident_offset < len(blob), 'Invalid signature identifier')

    identifier = bytes(blob[ident_offset:]).split(b'\0', 1)[0].decode('utf-8')
    algorithm = HASH_TYPES[hash_type][0]

    def digest(data):
        return hashlib.new(algorithm, data).digest()[:hash_size]

    def stored_hash(index):
        # Special slots sit before the code slots, so they use negative indexes.
        start = hash_offset + index * hash_size
        return bytes(blob[start:start + hash_size])

    bad_pages = []
    for page in range(code_slots):
        start = page * page_size
        end = min(code_limit, start + page_size)
        if digest(macho[start:end]) != stored_hash(page):
            bad_pages.append(page)
    require(not bad_pages, 'Code page hash mismatch: ' + str(bad_pages[:16]))

    checked_embedded = []
    checked_external = []
    unchecked_external = []
    for special in range(1, special_slots + 1):
        expected = stored_hash(-special)
        if special in embedded:
            require(digest(embedded[special]) == expected, 'Embedded special slot hash mismatch: ' + str(special))
            checked_embedded.append(special)
        elif external is not None and special in external:
            require(digest(external[special]) == expected, 'External special slot hash mismatch: ' + str(special))
            checked_external.append(special)
        elif any(expected):
            unchecked_external.append(special)
    if external is not None:
        require(set(external) <= set(checked_external), 'Bundle resources are not sealed by the main executable')

    return {
        'slot': slot,
        'identifier': identifier,
        'version': hex(version),
        'flags': hex(flags),
        'cdhash': hashlib.new(algorithm, blob).digest()[:20].hex(),
        'ad_hoc': bool(flags & CS_ADHOC),
        'code_limit': code_limit,
        'page_size': page_size,
        'hash_algorithm': algorithm,
        'checked_embedded_slots': checked_embedded,
        'checked_external_slots': checked_external,
        'unchecked_external_slots': unchecked_external,
    }


def inspect_slice(macho, external):
    """One Mach-O slice: find LC_CODE_SIGNATURE and check its CodeDirectories."""
    magic = bytes(macho[:4])
    require(magic in THIN_MAGIC, 'Expected a 64-bit Mach-O slice')
    endian = THIN_MAGIC[magic]
    require(len(macho) >= 32, 'Truncated Mach-O header')
    cpu_type = struct.unpack_from(endian + 'I', macho, 4)[0]
    file_type = struct.unpack_from(endian + 'I', macho, 12)[0]
    command_count, commands_size = struct.unpack_from(endian + 'II', macho, 16)
    part(macho, 32, commands_size)

    end = 32 + commands_size
    cursor = 32
    signature = None
    minimum_os = None
    for _ in range(command_count):
        require(cursor + 8 <= end, 'Truncated load command')
        command, size = struct.unpack_from(endian + 'II', macho, cursor)
        require(size >= 8 and cursor + size <= end, 'Invalid load command size')
        if command == LC_CODE_SIGNATURE:
            require(size >= 16 and signature is None, 'Invalid or duplicate LC_CODE_SIGNATURE')
            signature = struct.unpack_from(endian + 'II', macho, cursor + 8)
        elif command == LC_BUILD_VERSION:
            require(size >= 24, 'Invalid LC_BUILD_VERSION')
            minimum_os = format_version(struct.unpack_from(endian + 'I', macho, cursor + 12)[0])
        elif command == LC_VERSION_MIN_MACOSX:
            require(size >= 16, 'Invalid LC_VERSION_MIN_MACOSX')
            minimum_os = format_version(struct.unpack_from(endian + 'I', macho, cursor + 8)[0])
        cursor += size

    require(signature is not None, 'Missing LC_CODE_SIGNATURE')
    signature_offset, signature_size = signature
    signed = part(macho, signature_offset, signature_size)
    require(len(signed) >= 12, 'Truncated signature SuperBlob')
    magic, length, count = struct.unpack_from('>III', signed)
    require(magic == CSMAGIC_EMBEDDED_SIGNATURE, 'Invalid signature SuperBlob')
    require(12 + count * 8 <= length <= len(signed), 'Invalid signature SuperBlob length')
    signed = signed[:length]

    embedded = {}
    for index in range(count):
        slot, offset = struct.unpack_from('>II', signed, 12 + index * 8)
        require(slot not in embedded and offset >= 12 + count * 8, 'Invalid embedded signature slot')
        blob_length = struct.unpack_from('>I', part(signed, offset, 8), 4)[0]
        require(blob_length >= 8, 'Invalid signature blob length')
        embedded[slot] = part(signed, offset, blob_length)

    require(0 in embedded, 'Missing primary CodeDirectory')
    directories = []
    for slot, blob in embedded.items():
        # Slot 0 is the main CodeDirectory, 0x1000 to 0x1004 are the alternates.
        if slot == 0 or 0x1000 <= slot < 0x1005:
            directories.append(check_code_directory(macho, blob, slot, signature_offset, embedded, external))

    return {
        'cpu_type': cpu_type,
        'file_type': file_type,
        'minimum_os': minimum_os,
        'directories': directories,
    }


def inspect_bytes(data, external=None):
    """Check every slice of a thin or universal Mach-O file. Returns one dict per slice."""
    data = memoryview(data)
    magic = bytes(data[:4])

    try:
        if magic in THIN_MAGIC:
            return [inspect_slice(data, external)]

        require(magic in FAT_MAGIC and len(data) >= 8, 'Not a Mach-O binary')
        endian, wide = FAT_MAGIC[magic]
        count = struct.unpack_from(endian + 'I', data, 4)[0]
        require(0 < count <= 64, 'Invalid universal slice count')
        stride = 32 if wide else 20
        part(data, 8, count * stride)

        slices = []
        for index in range(count):
            layout = endian + ('QQ' if wide else 'II')
            offset, size = struct.unpack_from(layout, data, 8 + index * stride + 8)
            slices.append(inspect_slice(part(data, offset, size), external))
        return slices
    except struct.error:
        raise ValueError('Truncated Mach-O structure')


def bundle_resources(path):
    """Info.plist and CodeResources for a bundle's main executable, or None for a plain file."""
    if path.parent.name != 'MacOS' or path.parent.parent.name != 'Contents':
        return None

    contents = path.parent.parent
    info = contents / 'Info.plist'
    resources = contents / '_CodeSignature/CodeResources'
    if not info.is_file() or not resources.is_file():
        return None
    return {1: info.read_bytes(), 3: resources.read_bytes()}


def macho_files(paths):
    for root in paths:
        candidates = sorted(root.rglob('*')) if root.is_dir() else [root]
        for path in candidates:
            if not path.is_file():
                continue
            with path.open('rb') as stream:
                magic = stream.read(4)
            if magic in THIN_MAGIC or magic in FAT_MAGIC:
                yield path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=Path, help='Mach-O files or folders to scan')

    args = parser.parse_args()
    found = False
    failed = False

    for path in macho_files(args.paths):
        found = True
        try:
            slices = inspect_bytes(path.read_bytes(), bundle_resources(path))
        except (ValueError, OSError) as error:
            failed = True
            print('FAIL ' + str(path) + ': ' + str(error))
            continue
        print('ok   ' + str(path) + ' (' + ', '.join(hex(s['cpu_type']) for s in slices) + ')')

    if not found:
        print('No Mach-O files found')
        return 1
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
