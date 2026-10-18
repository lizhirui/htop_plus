#!/usr/bin/env python3
"""Integration tests: real C modules, fake NVML, and a simulated serial device."""
import os
import json
from pathlib import Path
import pty
import select
import subprocess
import threading
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'tests' / 'build'
SAMPLE = b'12.06,0.48,12.06,0.44,12.06,0.40,12.05,0.36,12.04,0.32,12.03,0.28,27.48,42.23,43.50\n'


def build():
    BUILD.mkdir(exist_ok=True)
    subprocess.run(['/usr/bin/gcc', '-std=c99', '-D_DEFAULT_SOURCE', '-I.', '-Ilinux',
                    'tests/HardwareProbe.c', 'linux/NvidiaTelemetry.c',
                    'linux/PowerCable.c', 'linux/CableConfig.c', 'XUtils.c', '-ldl', '-lm', '-ljson-c',
                    '-o', str(BUILD / 'hardware-probe')], cwd=ROOT, check=True)
    subprocess.run(['/usr/bin/gcc', '-shared', '-fPIC', 'tests/FakeNvml.c',
                    '-o', str(BUILD / 'libnvidia-ml.so.1')], cwd=ROOT, check=True)
    subprocess.run(['/usr/bin/gcc', '-std=c99', '-D_DEFAULT_SOURCE', '-I.', '-Ilinux',
                    'tests/ConfigProbe.c', 'linux/CableConfig.c', 'XUtils.c', '-ljson-c',
                    '-o', str(BUILD / 'config-probe')], cwd=ROOT, check=True)


class HardwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build()

    def test_nvml_units_rpm_and_unsupported_field(self):
        result = subprocess.check_output([str(BUILD / 'hardware-probe')],
                    env=dict(os.environ, LD_LIBRARY_PATH=str(BUILD)), text=True)
        self.assertIn('gpus=2', result)
        self.assertIn('NVIDIA GeForce RTX 2080 Ti available=1 usage=42.0', result)
        self.assertIn('NVIDIA GeForce RTX 5090 available=1 usage=nan', result)
        self.assertIn('memory=20480/32768MiB', result)
        self.assertIn('fan=35% rpm=1200 power=123.45/600.00W', result)

    def test_nvml_initialization_failure(self):
        result = subprocess.check_output([str(BUILD / 'hardware-probe')],
                    env=dict(os.environ, LD_LIBRARY_PATH=str(BUILD), TEST_NVML_INIT_FAIL='1'), text=True)
        self.assertIn('gpus=0 message=NVIDIA driver initialization failed', result)

    def cable(self, sample=SAMPLE, identity=b'WINGSTUDIO 12V-2X6 Ti V1.0\n\r\n',
              silent=False, disconnect=False, config_timeout=None):
        master, slave = pty.openpty()
        port = os.ttyname(slave)
        # The module refuses already-open serial devices, including this fd.
        os.close(slave)
        stop = threading.Event()
        commands = []

        def device():
            buffered = b''
            while not stop.is_set():
                if not select.select([master], [], [], 0.02)[0]:
                    continue
                try:
                    data = os.read(master, 1024)
                except OSError:
                    time.sleep(0.01)
                    continue
                buffered += data
                while b'\n' in buffered:
                    command, buffered = buffered.split(b'\n', 1)
                    commands.append(command)
                    if silent:
                        continue
                    if command == b'*IDN?':
                        response = identity
                    elif command == b'SENSORDATA?':
                        response = sample
                    else:
                        return
                    # Exercise responses split across separate UI refreshes.
                    os.write(master, response[:7])
                    time.sleep(0.08)
                    os.write(master, response[7:])
                    if disconnect and command == b'SENSORDATA?':
                        time.sleep(0.2)
                        os.close(master)
                        return

        worker = threading.Thread(target=device, daemon=True)
        worker.start()
        start = time.monotonic()
        environment = dict(os.environ, HTOP_POWER_CABLE_PORT=port)
        temporary = tempfile.TemporaryDirectory()
        if config_timeout is not None:
            path = Path(temporary.name) / 'config.json'
            path.write_text(json.dumps({'usb_serial': None, 'port': port,
                                        'timeout_seconds': config_timeout, 'baudrate': 115200}))
            environment.pop('HTOP_POWER_CABLE_PORT', None)
            environment['HTOP_POWER_CABLE_CONFIG'] = str(path)
        try:
            result = subprocess.run([str(BUILD / 'hardware-probe'), '100' if silent else '30', 'cable'],
                        env=environment, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=7, check=True)
        finally:
            stop.set()
            worker.join(timeout=1)
            if not disconnect:
                os.close(master)
            temporary.cleanup()
        self.assertLess(time.monotonic() - start, 6)
        self.assertTrue(all(c in (b'*IDN?', b'SENSORDATA?') for c in commands), commands)
        return result.stdout, commands

    def test_serial_fragmented_response_and_units(self):
        result, commands = self.cable()
        self.assertIn('valid=1 power=27.48 connector=42.23 controller=43.50', result)
        self.assertIn('channel A 12.06V 0.48A', result)
        self.assertIn('channel F 12.03V 0.28A', result)
        self.assertEqual(commands, [b'*IDN?', b'SENSORDATA?'])

    def test_wrong_identity_stops_sensor_queries(self):
        result, commands = self.cable(identity=b'OTHER DEVICE\n')
        self.assertIn('Cable: wrong device ID', result)
        self.assertEqual(commands, [b'*IDN?'])

    def test_invalid_sensor_fields(self):
        for sample in (b'1,2,3\n', SAMPLE.replace(b'12.06', b'nan', 1),
                       SAMPLE.replace(b'12.06', b'inf', 1), SAMPLE.rstrip() + b',99\n',
                       SAMPLE.replace(b'12.06', b'', 1), SAMPLE.replace(b'12.06', b'0x1p0', 1),
                       b'x' * 1100 + b'\n'):
            with self.subTest(sample=sample[:40]):
                result, _ = self.cable(sample=sample)
                self.assertNotIn('valid=1', result)
                self.assertRegex(result, 'invalid sensor response|oversized response')

    def test_timeout_does_not_block_polling(self):
        result, commands = self.cable(silent=True)
        self.assertIn('query timed out (4s)', result)
        self.assertEqual(commands, [b'*IDN?'])
        self.assertGreater(result.count('cable valid='), 80)

    def test_serial_connection_and_timeout_from_config(self):
        result, commands = self.cable(config_timeout=1)
        self.assertIn('valid=1 power=27.48', result)
        self.assertEqual(commands, [b'*IDN?', b'SENSORDATA?'])
        result, _ = self.cable(config_timeout=0.25, silent=True)
        self.assertIn('query timed out (0.25s)', result)

    def test_disconnect_clears_previous_validity(self):
        result, _ = self.cable(disconnect=True)
        self.assertIn('valid=1', result)
        self.assertIn('valid=0', result.split('valid=1', 1)[1])
        self.assertIn('Cable disconnected', result)

    def test_busy_port_not_opened(self):
        master, slave = pty.openpty()
        try:
            result = subprocess.check_output([str(BUILD / 'hardware-probe'), '1', 'cable'],
                        env=dict(os.environ, HTOP_POWER_CABLE_PORT=os.ttyname(slave)), text=True)
            self.assertIn('Cable busy', result)
            self.assertFalse(select.select([master], [], [], 0)[0])
        finally:
            os.close(master)
            os.close(slave)

    def test_disabled_and_missing_cable(self):
        for port, message in (('off', 'Cable disabled'),
                              ('/dev/htop_plus-device-does-not-exist', 'Cable unavailable')):
            with self.subTest(port=port):
                result = subprocess.check_output([str(BUILD / 'hardware-probe'), '1', 'cable'],
                            env=dict(os.environ, HTOP_POWER_CABLE_PORT=port), text=True)
                self.assertIn('valid=0', result)
                self.assertIn(message, result)

    def resolve(self, config, devices=(), raw=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sysroot = root / 'sys'
            devroot = root / 'dev'
            sysroot.mkdir()
            devroot.mkdir()
            for name, serial, vid, pid in devices:
                usb = root / 'usb' / name
                interface = usb / 'interface'
                interface.mkdir(parents=True)
                for key, value in (('serial', serial), ('idVendor', vid), ('idProduct', pid)):
                    (usb / key).write_text(value + '\n')
                tty = sysroot / name
                tty.mkdir()
                (tty / 'device').symlink_to(interface)
                (devroot / name).touch()
            path = root / 'config.json'
            path.write_text(json.dumps(config) if raw is None else raw)
            return subprocess.run([str(BUILD / 'config-probe'), str(path), str(sysroot), str(devroot)],
                                  text=True, stdout=subprocess.PIPE, check=False)

    def test_config_serial_selects_current_port_and_usb_ids(self):
        config = {'usb_serial': 'wanted', 'usb_vid': '1A86', 'usb_pid': '55D3',
                  'port': '/dev/ttyACM0', 'baudrate': 115200, 'timeout_seconds': 2.5}
        result = self.resolve(config, (('ttyACM0', 'other', '1a86', '55d3'),
                                       ('ttyACM7', 'wanted', '1a86', '55d3')))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('/dev/ttyACM7 serial=wanted vid=1a86 pid=55d3 timeout=2.5', result.stdout)

    def test_config_no_serial_match_never_falls_back(self):
        config = {'usb_serial': 'missing', 'port': '/dev/ttyACM0'}
        result = self.resolve(config, (('ttyACM0', 'other', '1a86', '55d3'),))
        self.assertEqual(result.returncode, 1)
        self.assertIn('no fallback', result.stdout)

    def test_config_duplicate_serial_and_wrong_vid(self):
        devices = (('ttyACM0', 'same', '1a86', '55d3'), ('ttyACM1', 'same', '1a86', '55d3'))
        result = self.resolve({'usb_serial': 'same'}, devices)
        self.assertEqual(result.returncode, 1)
        self.assertIn('multiple ports', result.stdout)
        result = self.resolve({'usb_serial': 'same', 'usb_vid': '1234'}, devices)
        self.assertEqual(result.returncode, 1)
        self.assertIn('no fallback', result.stdout)

    def test_config_null_serial_uses_explicit_port(self):
        result = self.resolve({'usb_serial': None, 'port': '/dev/ttyACM9'})
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('port=/dev/ttyACM9', result.stdout)

    def test_config_invalid_json_and_fields(self):
        for raw in ('{} garbage', '{"port":"/dev/ttyACM0",}', '[]'):
            with self.subTest(raw=raw):
                self.assertEqual(self.resolve({}, raw=raw).returncode, 1)
        for extra in ({'timeout_seconds': 0}, {'timeout_seconds': True}, {'timeout_seconds': None},
                      {'baudrate': 9600}, {'baudrate': '115200'}, {'usb_vid': 'zzzz'},
                      {'usb_serial': ''}, {'usb_serial': 123}, {'port': 'ttyACM0'}, {'unknown': 1}):
            with self.subTest(extra=extra):
                result = self.resolve(dict({'port': '/dev/ttyACM0'}, **extra))
                self.assertEqual(result.returncode, 1, result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
