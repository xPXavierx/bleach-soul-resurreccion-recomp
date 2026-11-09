import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('verify', Path(__file__).parents[1] / 'verify-setup.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class VerificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.context = {'source_commit': 'a' * 40, 'recipe_commit': 'b' * 40,
                        'run_url': 'https://example.invalid/test', 'release': None}
        self.a = self.make_build('a', b'installer-one')
        self.b = self.make_build('b', b'installer-one')

    def make_build(self, name, data):
        folder = self.root / name
        folder.mkdir(exist_ok=True)
        exe = folder / verify.NAME
        exe.write_bytes(data)
        verify.write_json(folder / 'build.json', {
            'source_commit': self.context['source_commit'], 'recipe_commit': self.context['recipe_commit'],
            'installer': {'sha256': verify.digest(exe), 'size': len(data)}})
        return folder

    def run_comparison(self, published=None):
        with contextlib.redirect_stdout(io.StringIO()):
            result = verify.compare(self.context, self.a, self.b, self.root / 'evidence', published)
        report = json.loads((self.root / 'evidence/verification.json').read_text())
        return result, report

    def published(self, data):
        file = self.root / 'published.exe'
        file.write_bytes(data)
        self.context['release'] = {'asset': {'size': len(data), 'digest': 'sha256:' + verify.digest(file)}}
        return file

    def test_two_builds_do_not_claim_release_verification(self):
        code, report = self.run_comparison()
        self.assertEqual(code, 0)
        self.assertEqual(report['status'], 'reproducible-build')
        self.assertIsNone(report['published_matches'])

    def test_all_three_identical(self):
        code, report = self.run_comparison(self.published(b'installer-one'))
        self.assertEqual((code, report['status']), (0, 'verified'))
        self.assertTrue(report['published_matches'])

    def test_mismatch_between_clean_builds_fails(self):
        self.make_build('b', b'installer-two')
        code, report = self.run_comparison()
        self.assertEqual(code, 1)
        self.assertEqual(report['status'], 'builds-differ')

    def test_published_mismatch_fails_with_evidence(self):
        code, report = self.run_comparison(self.published(b'old-installer'))
        self.assertEqual(code, 1)
        self.assertEqual(report['status'], 'published-installer-differs')
        self.assertTrue((self.root / 'evidence/summary.md').exists())

    def test_tampered_binary_cannot_use_manifest_hash(self):
        (self.b / verify.NAME).write_bytes(b'tampered')
        code, report = self.run_comparison()
        self.assertEqual((code, report['status']), (1, 'error'))

    def test_wrong_source_commit_is_rejected(self):
        self.context['source_commit'] = 'c' * 40
        code, report = self.run_comparison()
        self.assertEqual((code, report['status']), (1, 'error'))

    def test_missing_build_is_rejected_and_logged(self):
        (self.b / verify.NAME).unlink()
        code, report = self.run_comparison()
        self.assertEqual((code, report['status']), (1, 'error'))

    def test_github_digest_mismatch_is_rejected(self):
        published = self.published(b'installer-one')
        self.context['release']['asset']['digest'] = 'sha256:' + '0' * 64
        code, report = self.run_comparison(published)
        self.assertEqual((code, report['status']), (1, 'error'))

    def test_latest_resolves_the_release_tag_not_main(self):
        release = {'id': 123, 'tag_name': 'v1.1', 'target_commitish': 'main',
                   'html_url': 'https://example.invalid/v1.1', 'draft': False,
                   'assets': [{'name': verify.ASSET, 'id': 456, 'size': 13}]}
        with patch.object(verify, 'api', return_value=release) as request, \
             patch.object(verify.subprocess, 'run'), \
             patch.object(verify.subprocess, 'check_output', return_value='c' * 40 + '\n') as git, \
             patch.dict(verify.os.environ, {}, clear=True):
            result = verify.resolve('owner/repo', '@latest', 'a' * 40, 'b' * 40, self.root / 'source.json')
        request.assert_called_once_with('repos/owner/repo/releases/latest')
        git.assert_called_once_with(['git', 'rev-parse', '--verify', 'refs/tags/v1.1^{commit}'], text=True)
        self.assertEqual(result['source_commit'], 'c' * 40)
        self.assertEqual(result['release']['tag'], 'v1.1')


if __name__ == '__main__':
    unittest.main()
