"""Resolve immutable source/asset identities and compare complete installer bytes.

No executable being compared is ever run. A mismatch is a nonzero exit status.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import urllib.parse
import urllib.request
import zipfile

NAME = 'Bleach Soul Resurrección - Setup.exe'
ASSET = 'Bleach.Soul.Resurreccion.-.Setup.exe'


def digest(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def api(path):
    headers = {'Accept': 'application/vnd.github+json', 'User-Agent': 'bleach-installer-verification',
               'X-GitHub-Api-Version': '2022-11-28'}
    if os.getenv('GH_TOKEN'):
        headers['Authorization'] = 'Bearer ' + os.environ['GH_TOKEN']
    with urllib.request.urlopen(urllib.request.Request('https://api.github.com/' + path, headers=headers), timeout=60) as response:
        return json.load(response)


def resolve(repository, tag, source_commit, recipe_commit, output):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Invalid repository')
    context = {'schema': 1, 'repository': repository, 'recipe_commit': recipe_commit,
               'source_commit': source_commit, 'release': None,
               'run_url': os.getenv('GITHUB_SERVER_URL', 'https://github.com') + '/' + repository +
                          '/actions/runs/' + os.getenv('GITHUB_RUN_ID', 'local')}
    if tag:
        release = api('repos/' + repository + '/releases/latest') if tag == '@latest' else None
        if release is not None:
            tag = release['tag_name']
        # Never use release.target_commitish (it can just say "main"). Resolve the tag itself.
        subprocess.run(['git', 'check-ref-format', 'refs/tags/' + tag], check=True)
        commit = subprocess.check_output(['git', 'rev-parse', '--verify', 'refs/tags/' + tag + '^{commit}'], text=True).strip()
        if release is None:
            release = api('repos/' + repository + '/releases/tags/' + urllib.parse.quote(tag, safe=''))
        assets = [a for a in release['assets'] if a['name'] == ASSET]
        if len(assets) != 1 or release['draft']:
            raise ValueError('Expected exactly one published installer named ' + ASSET)
        asset = assets[0]
        context['source_commit'] = commit
        context['release'] = {'id': release['id'], 'tag': tag, 'url': release['html_url'],
                              'asset': {k: asset.get(k) for k in
                                        ('id', 'name', 'size', 'digest', 'updated_at', 'browser_download_url')}}
    if not all(re.fullmatch(r'[0-9a-f]{40}', context[key]) for key in ('source_commit', 'recipe_commit')):
        raise ValueError('Expected full source and recipe commit IDs')
    write_json(output, context)
    if os.getenv('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as stream:
            stream.write('source_commit=' + context['source_commit'] + '\n')
    return context


def inventory(path):
    """Diagnostic only: raw SHA-256 comparisons never ignore metadata or signatures."""
    with open(path, 'rb') as stream:
        stream.seek(-16, 2)
        length, magic = struct.unpack('<q8s', stream.read(16))
        size = stream.tell()
        if magic != b'BLEACHPK' or not 0 < length < size - 16:
            raise ValueError('Invalid installer footer')
        stream.seek(0)
        stub = stream.read(size - 16 - length)
        payload = stream.read(length)
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        if len(set(archive.namelist())) != len(archive.namelist()):
            raise ValueError('Duplicate ZIP entries')
        files = {}
        for info in archive.infolist():
            if info.file_size > 1024 ** 3:
                raise ValueError('Unexpected payload file size')
            with archive.open(info) as stream:
                files[info.filename] = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'stub_sha256': hashlib.sha256(stub).hexdigest(), 'files': files}


def differences(left, right):
    return {name: {'left': left.get(name), 'right': right.get(name)}
            for name in sorted(left.keys() | right.keys()) if left.get(name) != right.get(name)}


def download_published(context, destination):
    expected = context['release']['asset']
    current = api('repos/' + context['repository'] + '/releases/assets/' + str(expected['id']))
    for key in ('id', 'name', 'size', 'digest', 'updated_at', 'browser_download_url'):
        if current.get(key) != expected.get(key):
            raise ValueError('Release asset changed during the run: ' + key)
    if not 0 < expected['size'] <= 1024 ** 3:
        raise ValueError('Unexpected release asset size')
    # Public download: never forward GH_TOKEN to a release redirect or another host.
    with urllib.request.urlopen(expected['browser_download_url'], timeout=60) as source, open(destination, 'wb') as target:
        remaining = expected['size']
        while chunk := source.read(min(1 << 20, remaining + 1)):
            remaining -= len(chunk)
            if remaining < 0:
                raise ValueError('Release asset larger than declared')
            target.write(chunk)
        if remaining:
            raise ValueError('Truncated release asset')


def compare(context, build_a, build_b, output, published=None):
    output.mkdir(parents=True, exist_ok=True)
    report = {'schema': 1, 'context': context, 'status': 'error', 'hashes': {},
              'builds_match': False, 'published_matches': None,
              'claim': 'No successful verification has been established.'}
    try:
        if os.getenv('BUILD_RESULT', 'success') != 'success':
            raise ValueError('At least one clean build job failed; no successful attestation is permitted')
        for label, folder in (('build_a', build_a), ('build_b', build_b)):
            exe = folder / NAME
            actual = digest(exe)
            report['hashes'][label] = actual
            manifest = json.loads((folder / 'build.json').read_text(encoding='utf-8'))
            if manifest['installer']['sha256'] != actual or manifest['installer']['size'] != exe.stat().st_size:
                raise ValueError(label + ': build manifest does not match the actual installer')
            for key in ('source_commit', 'recipe_commit'):
                if manifest[key] != context[key]:
                    raise ValueError(label + ': wrong ' + key)
            shutil.copy2(folder / 'build.json', output / (label + '.json'))
        report['builds_match'] = report['hashes']['build_a'] == report['hashes']['build_b']
        if context.get('release'):
            if published is None:
                published = output / 'published.exe'
                download_published(context, published)
            actual = digest(published)
            report['hashes']['published'] = actual
            asset = context['release']['asset']
            if published.stat().st_size != asset['size']:
                raise ValueError('Published installer size differs from release metadata')
            if asset.get('digest') and asset['digest'] != 'sha256:' + actual:
                raise ValueError('Published installer differs from GitHub asset digest')
            report['published_matches'] = report['builds_match'] and actual == report['hashes']['build_a']
        if not report['builds_match']:
            report['status'] = 'builds-differ'
        elif context.get('release') and not report['published_matches']:
            report['status'] = 'published-installer-differs'
        else:
            report['status'] = 'verified' if context.get('release') else 'reproducible-build'
            report['claim'] = ('Both clean builds and the downloaded published installer have identical SHA-256.'
                               if context.get('release') else
                               'Two clean builds have identical SHA-256. No published installer was compared.')
        if report['status'] in ('builds-differ', 'published-installer-differs'):
            report['claim'] = 'SHA-256 mismatch. This run does not attest equality with the published installer.'
            try:
                left = inventory(build_a / NAME)
                other = inventory(build_b / NAME if not report['builds_match'] else published)
                write_json(output / 'payload-differences.json', {
                    'diagnostic_only': True, 'stub_matches': left['stub_sha256'] == other['stub_sha256'],
                    'files': differences(left['files'], other['files'])})
            except (ValueError, OSError, zipfile.BadZipFile) as error:
                report['diagnostic_error'] = str(error)
    except Exception as error:
        report['status'] = 'error'
        report['error'] = str(error)
    write_json(output / 'verification.json', report)
    lines = ['# Installer verification', '', '**Result: ' + report['status'] + '**', '', report['claim'], '',
             '- Source commit: `' + context['source_commit'] + '`',
             '- Build recipe commit: `' + context['recipe_commit'] + '`',
             '- Run: ' + context['run_url'], '', '| File | SHA-256 |', '| --- | --- |']
    lines += ['| ' + label + ' | `' + value + '` |' for label, value in report['hashes'].items()]
    if report.get('error'):
        lines += ['', 'Error: ' + report['error']]
    lines += ['', 'This checks byte equality and build provenance. It is not a malware or code safety certification.', '']
    summary = '\n'.join(lines)
    (output / 'summary.md').write_text(summary, encoding='utf-8')
    if os.getenv('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as stream:
            stream.write(summary)
    print(summary)
    return 0 if report['status'] in ('verified', 'reproducible-build') else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    resolve_args = commands.add_parser('resolve')
    resolve_args.add_argument('--repository', required=True)
    resolve_args.add_argument('--tag', default='')
    resolve_args.add_argument('--source-commit', required=True)
    resolve_args.add_argument('--recipe-commit', required=True)
    resolve_args.add_argument('--output', type=Path, required=True)
    compare_args = commands.add_parser('compare')
    compare_args.add_argument('--context', type=Path, required=True)
    compare_args.add_argument('--build-a', type=Path, required=True)
    compare_args.add_argument('--build-b', type=Path, required=True)
    compare_args.add_argument('--output', type=Path, required=True)
    compare_args.add_argument('--published', type=Path, help='Local copy for offline comparison')
    args = vars(parser.parse_args())
    command = args.pop('command')
    if command == 'resolve':
        resolve(**args)
    else:
        args['context'] = json.loads(args['context'].read_text(encoding='utf-8'))
        raise SystemExit(compare(**args))
