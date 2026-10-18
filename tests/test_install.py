#!/usr/bin/env python3
"""Exercise the actual installer/launcher/uninstaller inside a temporary root."""
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='htop_plus install ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.stage = self.root / 'staging root'
        self.config = self.root / 'user config'
        self.prefix = '/usr/local'
        self.environment = dict(os.environ, DESTDIR=str(self.stage), XDG_CONFIG_HOME=str(self.config))
        self.environment.pop('HTOPRC', None)
        self.environment.pop('PREFIX', None)

    def install(self):
        subprocess.run([str(ROOT / 'install.sh'), '--no-build', '--prefix', self.prefix],
                       env=self.environment, check=True, stdout=subprocess.PIPE, text=True)
        return self.stage / self.prefix.lstrip('/')

    def test_installed_launcher_uses_private_binary_and_preserves_configuration(self):
        target = self.install()
        original = target / 'bin' / 'htop'
        original.write_text('#!/bin/sh\nexit 42\n')
        original.chmod(0o755)
        launcher = target / 'bin' / 'htop_plus'
        result = subprocess.check_output([str(launcher), '--version'], env=self.environment, text=True)
        self.assertIn('htop ', result)
        layout = self.config / 'htop_plus' / 'htoprc'
        self.assertEqual(layout.read_bytes(), (ROOT / 'htoprc.example').read_bytes())
        self.assertEqual(stat.S_IMODE(layout.stat().st_mode), 0o600)
        layout.write_text('# custom user layout\n')
        self.install()
        subprocess.run([str(launcher), '--version'], env=self.environment, check=True, stdout=subprocess.PIPE)
        self.assertEqual(layout.read_text(), '# custom user layout\n')
        self.assertEqual(original.read_text(), '#!/bin/sh\nexit 42\n')
        self.assertEqual(stat.S_IMODE((target / 'libexec/htop_plus/htop').stat().st_mode), 0o755)

    def test_explicit_htoprc_and_custom_prefix(self):
        self.prefix = '/opt/custom prefix'
        target = self.install()
        explicit = self.root / 'explicit htoprc'
        explicit.write_text('# explicitly chosen layout\n')
        environment = dict(self.environment, HTOPRC=str(explicit))
        result = subprocess.check_output([str(target / 'bin/htop_plus'), '--version'], env=environment, text=True)
        self.assertIn('htop ', result)
        self.assertFalse(self.config.exists())
        self.assertEqual(explicit.read_text(), '# explicitly chosen layout\n')

    def test_uninstall_preserves_user_device_and_unrelated_files(self):
        target = self.install()
        subprocess.run([str(target / 'bin/htop_plus'), '--version'],
                       env=self.environment, check=True, stdout=subprocess.PIPE)
        cable = self.stage / 'etc/nvpowermon/config.json'
        cable.parent.mkdir(parents=True)
        cable.write_text('{"usb_serial":"preserve"}\n')
        unrelated = target / 'share/doc/htop_plus/my-note.txt'
        unrelated.write_text('preserve this file\n')
        subprocess.run([str(target / 'share/htop_plus/uninstall.sh'), '--prefix', self.prefix],
                       env=self.environment, check=True, stdout=subprocess.PIPE)
        for name in ('bin/htop_plus', 'libexec/htop_plus/htop', 'share/htop_plus/htoprc.example',
                     'share/htop_plus/uninstall.sh', 'share/doc/htop_plus/COPYING',
                     'share/doc/htop_plus/README.md', 'share/man/man1/htop_plus.1'):
            self.assertFalse((target / name).exists(), name)
        self.assertTrue((self.config / 'htop_plus/htoprc').is_file())
        self.assertEqual(cable.read_text(), '{"usb_serial":"preserve"}\n')
        self.assertEqual(unrelated.read_text(), 'preserve this file\n')


if __name__ == '__main__':
    unittest.main(verbosity=2)
