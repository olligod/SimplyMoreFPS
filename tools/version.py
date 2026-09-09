#!/usr/bin/env python3
"""Update the mod version and README date together."""
import argparse
import datetime
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MONTHS = ('January', 'February', 'March', 'April', 'May', 'June', 'July', 'August',
          'September', 'October', 'November', 'December')


def update(version):
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version):
        raise ValueError('Use a version like 0.3.9')
    if any(int(part) > 65534 for part in version.split('.')):
        raise ValueError('Each version number must be at most 65534')

    today = datetime.date.today()
    date = str(today.day) + ' ' + MONTHS[today.month - 1] + ' ' + str(today.year)
    changes = (
        ('About/About.xml', r'(?<=<modVersion>)[^<]+(?=</modVersion>)', version),
        ('src/Smf.Mod/Smf.Mod.csproj', r'(?<=<Version>)[^<]+(?=</Version>)', version),
        ('README.md', r'Version [0-9.]+\. Last updated: [^\n]+', 'Version ' + version + '. Last updated: ' + date + '.'),
    )
    updated = []
    for relative, pattern, replacement in changes:
        path = ROOT / relative
        content, count = re.subn(pattern, lambda _: replacement, path.read_text(encoding='utf-8'))
        if count != 1:
            raise ValueError('Expected one version entry in ' + relative)
        updated.append((path, content))

    for path, content in updated:
        path.write_text(content, encoding='utf-8', newline='\n')
    print('Version ' + version + ' set. Add the release notes to CHANGELOG.md.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('version')
    args = parser.parse_args()
    try:
        update(args.version)
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')
