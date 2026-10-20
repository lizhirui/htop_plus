#!/usr/bin/env python3
"""Exercise real C collectors with fake NVML and nvpowermon socket peers."""
from contextlib import ExitStack
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'tests' / 'build'


def build():
    BUILD.mkdir(exist_ok=True)
    subprocess.run(['/usr/bin/gcc', '-std=c99', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra',
                    '-I.', '-Ilinux', 'tests/HardwareProbe.c', 'linux/NvidiaTelemetry.c',
                    'linux/PowerCable.c', 'XUtils.c', '-ldl', '-lm', '-ljson-c',
                    '-o', str(BUILD / 'hardware-probe')], cwd=ROOT, check=True)
    subprocess.run(['/usr/bin/gcc', '-shared', '-fPIC', 'tests/FakeNvml.c',
                    '-o', str(BUILD / 'libnvidia-ml.so.1')], cwd=ROOT, check=True)


def snapshot(sequence=1, power=27.48, age=0):
    return {
        'schema_version': 1, 'sequence': sequence, 'time': '2026-10-20T12:00:00+08:00',
        'state': 'ok', 'valid': True, 'sample_monotonic_s': time.monotonic() - age,
        'device': {'port': '/dev/ttyACM7', 'usb_serial': 'example'},
        'id': 'WINGSTUDIO 12V-2X6 Ti V1.0',
        'status': {
            'channels': [{'channel': chr(65 + i), 'voltage_v': v, 'current_a': a}
                         for i, (v, a) in enumerate(((12.06, .48), (12.06, .44), (12.06, .40),
                                                    (12.05, .36), (12.04, .32), (12.03, .28)))],
            'total_power_w': power, 'cable_temperature_c': 42.23, 'board_temperature_c': 43.5,
        },
        'device_config': dict(zip((
            'screen_rotation', 'shunt_resistance_milliohm', 'current_correction_rate',
            'fan_control_type', 'fan_on_temperature_c', 'alarm_temperature_c',
            'alarm_current_per_channel_a', 'alarm_timeout_s', 'protection_current_per_channel_a',
            'protection_temperature_c'), (1, 1.0, 1.0, 0, 45, 85, 12, 120, 15, 100))),
        'error': None,
    }


def unavailable(state, sequence):
    return dict(schema_version=1, sequence=sequence, state=state, valid=False,
                time='2026-10-20T12:00:00+08:00', sample_monotonic_s=None,
                device=None, id=None, status=None, device_config=None,
                error='串口已断开' if state == 'error' else None)


def frame(data):
    return json.dumps(data, ensure_ascii=False).encode() + b'\n'


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

    def cable(self, exchange=None, loops=12, connections=1, variable='HTOP_POWER_CABLE_SOCKET',
              clients=1):
        """Run native subscribers; assert that none sends any byte to the service."""
        with tempfile.TemporaryDirectory(prefix='htop-cable-') as directory:
            path = str(Path(directory) / 'status.sock')
            stop = threading.Event()
            errors = []
            received = []
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(path)
                server.listen(8)
                server.settimeout(.1)

                def peer():
                    try:
                        with ExitStack() as peers:
                            active = []
                            for number in range(connections):
                                while not stop.is_set():
                                    try:
                                        client, _ = server.accept()
                                        break
                                    except socket.timeout:
                                        continue
                                else:
                                    return
                                peers.enter_context(client)
                                active.append(client)
                                if exchange is None:
                                    client.sendall(frame(snapshot()))
                                else:
                                    exchange(client, number)
                                if number + 1 < connections and clients == 1:
                                    # Simulate service restart, leaving the listener up.
                                    client.close()
                                    active.remove(client)
                            while active and not stop.is_set():
                                for client in select.select(active, [], [], .05)[0]:
                                    data = client.recv(1024)
                                    if not data:
                                        active.remove(client)
                                    else:
                                        received.append(data)
                    except (BrokenPipeError, ConnectionResetError):
                        pass  # A malformed frame must cause the subscriber to close.
                    except Exception as error:
                        errors.append(error)

                worker = threading.Thread(target=peer, daemon=True)
                worker.start()
                environment = dict(os.environ)
                for key in ('HTOP_POWER_CABLE_PORT', 'HTOP_POWER_CABLE_CONFIG',
                            'HTOP_POWER_CABLE_SOCKET', 'NVPOWERMON_SOCKET'):
                    environment.pop(key, None)
                environment[variable] = path
                processes = []
                started = time.monotonic()
                try:
                    for _ in range(clients):
                        processes.append(subprocess.Popen(
                            [str(BUILD / 'hardware-probe'), str(loops), 'cable'],
                            env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
                    outputs = []
                    for process in processes:
                        output, error = process.communicate(timeout=loops * .05 + 4)
                        self.assertEqual(process.returncode, 0, error)
                        outputs.append(output)
                finally:
                    stop.set()
                    for process in processes:
                        if process.poll() is None:
                            process.kill()
                            process.communicate()
                    worker.join(timeout=2)
                self.assertFalse(worker.is_alive(), 'mock service did not stop')
                self.assertFalse(errors, errors)
                self.assertEqual(received, [], 'subscriber sent commands')
                self.assertLess(time.monotonic() - started, loops * .05 + 3)
                return outputs if clients > 1 else outputs[0]

    def test_fragmented_response_units_and_environment_fallback(self):
        def exchange(client, _):
            data = frame(snapshot())
            client.sendall(data[:7])
            time.sleep(.08)
            client.sendall(data[7:])
        result = self.cable(exchange, variable='NVPOWERMON_SOCKET')
        self.assertIn('valid=1 power=27.48 connector=42.23 controller=43.50', result)
        self.assertIn('channel A 12.06V 0.48A', result)
        self.assertIn('channel F 12.03V 0.28A', result)

    def test_coalesced_frames_and_unknown_fields(self):
        def exchange(client, _):
            data = snapshot(2, power=40)
            data['future_extension'] = {'ignored': True}
            client.sendall(frame(snapshot()) + frame(data))
        result = self.cable(exchange)
        self.assertIn('valid=1 power=40.00', result)

    def test_starting_error_and_recovery_without_reconnecting(self):
        def exchange(client, _):
            for data in (unavailable('starting', 1), snapshot(2),
                         unavailable('error', 3), snapshot(4, power=50)):
                client.sendall(frame(data))
                time.sleep(.12)
        result = self.cable(exchange, loops=16)
        self.assertIn('nvpowermon starting', result)
        self.assertIn('valid=1 power=27.48', result)
        self.assertIn('valid=0 power=0.00 connector=0.00 controller=0.00 message=Cable: 串口已断开', result)
        self.assertIn('valid=1 power=50.00', result)

    def test_invalid_metadata_and_required_fields(self):
        changes = [
            ('schema_version', 2), ('schema_version', True), ('sequence', 0), ('sequence', True),
            ('valid', 1), ('valid', False), ('state', 'other'), ('id', 'OTHER DEVICE'),
            ('device', None), ('status', None), ('device_config', None), ('time', 123),
            ('device', []), ('status', 12), ('device_config', []),
            ('sample_monotonic_s', True), ('sample_monotonic_s', float('nan')),
        ]
        for key, value in changes:
            with self.subTest(key=key, value=value):
                def exchange(client, _, key=key, value=value):
                    data = snapshot()
                    data[key] = value
                    client.sendall(frame(data))
                result = self.cable(exchange, loops=4)
                self.assertNotIn('valid=1', result)
                self.assertRegex(result, 'invalid nvpowermon|unsupported nvpowermon')
        for location, key, value in (
                ('status', 'total_power_w', float('inf')),
                ('status', 'board_temperature_c', '43.5'),
                ('device_config', 'alarm_temperature_c', True),
                ('device_config', 'fan_control_type', 9)):
            with self.subTest(location=location, key=key):
                def exchange(client, _, location=location, key=key, value=value):
                    data = snapshot()
                    data[location][key] = value
                    client.sendall(frame(data))
                self.assertNotIn('valid=1', self.cable(exchange, loops=4))

    def test_invalid_channels(self):
        for change in ('missing', 'swapped', 'bool', 'null'):
            with self.subTest(change=change):
                def exchange(client, _):
                    data = snapshot()
                    channels = data['status']['channels']
                    if change == 'missing':
                        channels.pop()
                    elif change == 'swapped':
                        channels.reverse()
                    elif change == 'bool':
                        channels[0]['current_a'] = True
                    else:
                        channels[0] = None
                    client.sendall(frame(data))
                result = self.cable(exchange, loops=4)
                self.assertNotIn('valid=1', result)
                self.assertIn('invalid nvpowermon', result)

    def test_invalid_json_utf8_and_frame_size(self):
        for data in (b'{} garbage\n', b'[]\n', b'{"schema_version":1,}\n',
                     b'\xff\n', b'{}\0\n', b'\n', frame(snapshot()).rstrip() + b'{}\n', b'x' * 65536):
            with self.subTest(data=data[:30]):
                result = self.cable(lambda client, _: client.sendall(data), loops=4)
                self.assertNotIn('valid=1', result)
                self.assertRegex(result, 'invalid nvpowermon JSON|oversized nvpowermon frame')

    def test_frame_size_exact_limit(self):
        def exchange(client, _):
            data = snapshot()
            data['padding'] = ''
            data['padding'] = 'x' * (65536 - len(frame(data)))
            self.assertEqual(len(frame(data)), 65536)
            client.sendall(frame(data))
        self.assertIn('valid=1', self.cable(exchange))

    def test_invalid_frame_clears_previous_reading(self):
        def exchange(client, _):
            client.sendall(frame(snapshot()))
            time.sleep(.15)
            client.sendall(b'broken\n')
        result = self.cable(exchange)
        self.assertIn('valid=1', result)
        self.assertIn('valid=0', result.split('valid=1', 1)[1])
        self.assertIn('invalid nvpowermon JSON', result)

    def test_stale_cached_and_future_samples(self):
        for age in (11, -5):
            with self.subTest(age=age):
                result = self.cable(lambda client, _: client.sendall(frame(snapshot(age=age))), loops=4)
                self.assertNotIn('valid=1', result)
                self.assertIn('stale data', result)

    def test_freshness_checked_without_new_frames(self):
        result = self.cable(lambda client, _: client.sendall(frame(snapshot(age=9.5))), loops=22)
        self.assertIn('valid=1', result)
        self.assertIn('stale data / no recent sample', result)
        self.assertIn('valid=0', result.split('valid=1', 1)[1])

    def test_old_sequences_ignored(self):
        def exchange(client, _):
            client.sendall(frame(snapshot(42, power=42)) + frame(snapshot(42, power=99)) +
                           frame(snapshot(1, power=99)))
        result = self.cable(exchange)
        self.assertIn('valid=1 power=42.00', result)
        self.assertNotIn('power=99.00', result)

    def test_disconnect_and_restart_reset_sequence(self):
        def exchange(client, number):
            client.sendall(frame(snapshot(42 if number == 0 else 1, power=42 if number == 0 else 55)))
            if number == 0:
                time.sleep(.15)
        result = self.cable(exchange, loops=125, connections=2)
        self.assertIn('valid=1 power=42.00', result)
        self.assertIn('nvpowermon disconnected', result)
        self.assertIn('valid=1 power=55.00', result)

    def test_parallel_subscribers(self):
        # The first exchange returns immediately so both peers are accepted.
        def exchange(client, number):
            client.sendall(frame(snapshot()))
            if number == 0:
                time.sleep(.2)
        outputs = self.cable(exchange, loops=12, connections=2, clients=2)
        for result in outputs:
            self.assertIn('valid=1 power=27.48', result)
            self.assertNotIn('disconnected', result)

    def test_silent_service_timeout_does_not_block_polling(self):
        result = self.cable(lambda client, _: None, loops=310)
        self.assertIn('subscription timed out', result)
        self.assertGreater(result.count('cable valid='), 300)

    def test_disabled_missing_socket_and_legacy_serial_override(self):
        for changes, message in (
                ({'HTOP_POWER_CABLE_SOCKET': 'off'}, 'Cable disabled'),
                ({'HTOP_POWER_CABLE_PORT': 'off'}, 'Cable disabled'),
                ({'HTOP_POWER_CABLE_SOCKET': '/tmp/htop_plus-no-such-service.sock'}, 'service unavailable'),
                ({'HTOP_POWER_CABLE_SOCKET': 'relative.sock'}, 'invalid nvpowermon socket path'),
                ({'HTOP_POWER_CABLE_PORT': '/dev/ttyACM0'}, 'configure the device in nvpowermon')):
            with self.subTest(changes=changes):
                env = {k: v for k, v in os.environ.items() if not k.startswith('HTOP_POWER_CABLE_')}
                result = subprocess.check_output([str(BUILD / 'hardware-probe'), '1', 'cable'],
                                                 env=dict(env, **changes), text=True)
                self.assertIn('valid=0', result)
                self.assertIn(message, result)


if __name__ == '__main__':
    unittest.main(verbosity=2)
