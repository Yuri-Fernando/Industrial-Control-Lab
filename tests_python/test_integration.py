"""Real TCP/Modbus + C++ control loop integration; no Docker or MQTT needed.

Every test talks to compiled C++ processes over real sockets:
  IntegrationTest          controller (C++ engine, local I/O) + gateway        — original baseline
  StEngineIntegrationTest  controller running the IEC 61131-3 ST block in the scan (CONTROL_ENGINE=st)
  RemoteIoIntegrationTest  controller + separate remote I/O node process (IO_MODE=remote)
Binaries are located by tests_python/binaries.py (PlatformIO .pio/build or ./build).
"""
import os
import json
import struct
import threading
from http.server import ThreadingHTTPServer
from urllib.request import Request, urlopen
import socket
import subprocess
import time
import unittest

from gateway.modbus_client import ModbusClient, ModbusError
from gateway.server import Gateway, make_handler
from tests_python.binaries import ROOT, controller_binary, io_node_binary


def wait_for(predicate, timeout=5, step=.08):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            if predicate(): return True
        except ModbusError:
            pass
        time.sleep(step)
    return False


def free_port():
    probe = socket.socket()
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def spawn(binary, **env):
    full_env = {**os.environ, **{k: str(v) for k, v in env.items()}}
    return subprocess.Popen([str(binary)], env=full_env, cwd=ROOT,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stop(process):
    if process is None or process.poll() is not None: return
    process.terminate()
    try: process.wait(timeout=3)
    except subprocess.TimeoutExpired: process.kill()


class ControllerFixture(unittest.TestCase):
    """Starts one controller (+ optional I/O node) and a gateway per test class."""
    engine = 'cpp'
    io_mode = 'local'

    @classmethod
    def setUpClass(cls):
        cls.port = free_port()
        cls.io_port = free_port()
        cls.io_process = None
        cls.process = None
        # Class cleanups also run when setUpClass fails (tearDownClass does not), so no process can leak.
        # They read the attribute at cleanup time because tests may restart the processes.
        cls.addClassCleanup(lambda: stop(cls.io_process))
        cls.addClassCleanup(lambda: stop(cls.process))
        if cls.io_mode == 'remote':
            cls.io_process = cls.start_io_node()
        cls.process = spawn(controller_binary(), CONTROL_BIND='127.0.0.1', CONTROL_PORT=cls.port,
                            CONTROL_ENGINE=cls.engine, IO_MODE=cls.io_mode, IO_PORT=cls.io_port)
        cls.modbus = ModbusClient(port=cls.port, timeout=.5)
        cls.addClassCleanup(cls.modbus.close)
        cls.gateway = Gateway(port=cls.port, mqtt_host='')
        cls.addClassCleanup(lambda: cls.gateway.close())
        cls.gateway.start()
        if not wait_for(lambda: cls.gateway.status().get('quality') == 'GOOD', 6):
            raise RuntimeError('Controller never became available')
        # START is refused until the I/O link is healthy, exactly like a PLC with a missing I/O module.
        if not wait_for(lambda: cls.modbus.read(4, 21, 1)[0] == 1, 6):
            raise RuntimeError('I/O link never became healthy')

    @classmethod
    def start_io_node(cls):
        return spawn(io_node_binary(), IO_BIND='127.0.0.1', IO_PORT=cls.io_port)

    def setUp(self):
        for item in ('low_flow', 'sensor_failure', 'overtemperature'):
            self.gateway.command(item, False)
        self.gateway.command('stop')
        self.assertTrue(wait_for(self._try_reset), 'cannot reset')

    def _try_reset(self):
        if self.current_state()[0] == 0: return True
        self.gateway.command('reset')
        return False

    def current_state(self):
        return self.modbus.read(4, 0, 6)

    def start_running(self):
        self.gateway.command('start')
        self.assertTrue(wait_for(lambda: self.current_state()[0] == 2), 'not running')


class IntegrationTest(ControllerFixture):
    def test_01_start_and_io(self):
        self.start_running()
        val = self.current_state()
        self.assertEqual(val[4], 1)
        self.assertGreater(val[3], 0)
        self.assertGreater(val[2], 0)
        self.assertEqual(val[5], 0)

    def test_02_low_flow_latch(self):
        self.start_running()
        self.gateway.command('low_flow', True)
        self.assertTrue(wait_for(lambda: self.current_state()[0] == 3))
        self.assertEqual(self.current_state()[5], 1)
        self.gateway.command('stop')
        time.sleep(.1)
        self.assertEqual(self.current_state()[0], 3, 'fault must remain latched')
        self.gateway.command('reset')
        time.sleep(.1)
        self.assertEqual(self.current_state()[0], 3, 'active fault injection blocks reset')
        self.gateway.command('low_flow', False)
        self.gateway.command('reset')
        self.assertTrue(wait_for(lambda: self.current_state()[0] == 0))

    def test_03_sensor_failure(self):
        self.start_running()
        self.gateway.command('sensor_failure', True)
        self.assertTrue(wait_for(lambda: self.current_state()[5] == 3))
        self.assertEqual(self.current_state()[4], 0)

    def test_04_overtemp(self):
        self.start_running()
        self.gateway.command('overtemperature', True)
        self.assertTrue(wait_for(lambda: self.current_state()[5] == 2))

    def test_05_emergency_stop(self):
        self.start_running()
        self.gateway.command('estop')
        self.assertTrue(wait_for(lambda: self.current_state()[0] == 4))
        self.assertEqual(self.current_state()[4], 0)
        self.gateway.command('start')
        time.sleep(.1)
        self.assertEqual(self.current_state()[0], 4)

    def test_06_setpoint_limits_and_modbus_exception(self):
        with self.assertRaises(ValueError): self.gateway.command('setpoint', 50)
        with self.assertRaises(ModbusError): self.modbus.write(1, 500)
        with self.assertRaises(ModbusError): self.modbus.read(4, 31, 2)
        self.gateway.command('setpoint', 31.5)
        self.assertEqual(self.modbus.read(3, 1, 1)[0], 315)

    def test_06b_fc16_is_atomic(self):
        def exchange(regs):
            function = bytes([16])
            pdu = function + struct.pack('>HHB', 1, len(regs), len(regs)*2)
            pdu += struct.pack('>'+'H'*len(regs), *regs)
            with socket.create_connection(('127.0.0.1', self.port), 2) as sock:
                sock.sendall(struct.pack('>HHHB', 44, 0, len(pdu)+1, 1)+pdu)
                head = ModbusClient._read_exact(sock, 7)
                length=struct.unpack('>HHHB', head)[2]
                return ModbusClient._read_exact(sock, length-1)
        self.assertEqual(exchange([295,80])[0], 16)
        self.assertEqual(self.modbus.read(3,1,2), (295,80))
        self.assertEqual(exchange([700, 50]), bytes([0x90,3]))
        self.assertEqual(self.modbus.read(3,1,2), (295,80), 'partial write occurred')

    def test_06c_rest_http(self):
        server=ThreadingHTTPServer(('127.0.0.1', 0), make_handler(self.gateway))
        worker=threading.Thread(target=server.serve_forever,daemon=True)
        worker.start()
        base=f'http://127.0.0.1:{server.server_port}'
        try:
            with urlopen(base+'/api/status',timeout=2) as r:
                data=json.load(r)
            self.assertEqual(data['quality'],'GOOD')
            with urlopen(base+'/metrics',timeout=2) as r:
                body = r.read().decode()
            self.assertIn('icl_temperature_c', body)
            self.assertIn('icl_scan_lateness_p99_us', body)
            with urlopen(base+'/api/history',timeout=2) as r:
                trend = json.load(r)
            self.assertGreater(len(trend), 0)
            self.assertIn('temperature_c', trend[-1])
            req=Request(base+'/api/command', data=b'{"command":"setpoint","value":28}',
                        method='POST',headers={'Content-Type':'application/json'})
            with urlopen(req,timeout=2) as r: self.assertTrue(json.load(r)['ok'])
            self.assertEqual(self.modbus.read(3,1,1)[0],280)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)

    def test_07_startup_flow_timeout(self):
        self.gateway.command('low_flow', True)
        self.gateway.command('start')
        self.assertTrue(wait_for(lambda: self.current_state()[0] == 3, timeout=4))
        self.assertEqual(self.current_state()[5], 1)

    def test_08_watchdog_trip(self):
        self.start_running()
        self.gateway.close()
        self.assertTrue(wait_for(lambda: self.current_state()[5] == 4, timeout=5))
        self.assertEqual(self.current_state()[4], 0)
        type(self).gateway = Gateway(port=self.port, mqtt_host='')
        self.gateway.start()
        self.assertTrue(wait_for(lambda: self.gateway.status().get('quality') == 'GOOD'))

    def test_09_gateway_decodes_telemetry(self):
        data=self.gateway.status()
        self.assertEqual(data['quality'],'GOOD')
        self.assertIn(data['state'], ('STOPPED','STARTING','RUNNING','FAULT','ESTOP'))
        self.assertIn('temperature_c',data)
        self.assertIn('max_scheduler_jitter_us',data)

    def test_10_timing_and_shadow_diagnostics(self):
        self.start_running()
        time.sleep(1.2)  # percentiles refresh once per second
        data = self.gateway.status()
        self.assertEqual(data['control_engine'], 'cpp')
        self.assertEqual(data['shadow_engine'], 'st')
        self.assertEqual(data['io_mode'], 'local')
        self.assertLessEqual(data['scan_lateness_p50_us'], data['scan_lateness_p99_us'])
        self.assertLessEqual(data['scan_lateness_p99_us'], data['scan_lateness_window_max_us'])
        self.assertEqual(data['engine_mismatches'], 0, 'C++ and ST engines diverged in the live loop')


class StEngineIntegrationTest(ControllerFixture):
    """The interlock state machine executing is st/FB_Interlocks.st, not C++."""
    engine = 'st'

    def test_01_engine_is_st_and_runs(self):
        self.start_running()
        status = self.modbus.read(4, 24, 4)
        self.assertEqual(status[0], 1, 'CONTROL_ENGINE register must report ST')
        self.assertEqual(status[3], 1, 'C++ shadow engine active')
        self.assertGreater(self.modbus.read(4, 3, 1)[0], 0, 'PI cooling active under ST engine')

    def test_02_st_latches_and_stop_cannot_clear(self):
        self.start_running()
        self.gateway.command('low_flow', True)
        self.assertTrue(wait_for(lambda: self.current_state()[0] == 3))
        self.gateway.command('stop')
        time.sleep(.15)
        self.assertEqual(self.current_state()[0], 3)
        self.gateway.command('low_flow', False)
        self.gateway.command('reset')
        self.assertTrue(wait_for(lambda: self.current_state()[0] == 0))

    def test_03_st_estop_and_startup_timeout(self):
        self.start_running()
        self.gateway.command('estop')
        self.assertTrue(wait_for(lambda: self.current_state()[0] == 4))
        self.gateway.command('reset')
        self.assertTrue(wait_for(lambda: self.current_state()[0] == 0))
        self.gateway.command('low_flow', True)
        self.gateway.command('start')
        self.assertTrue(wait_for(lambda: self.current_state()[5] == 1, timeout=4))

    def test_99_no_divergence_with_cpp_shadow(self):
        self.assertEqual(self.modbus.read(4, 26, 1)[0], 0)


class RemoteIoIntegrationTest(ControllerFixture):
    """Closed loop crosses a process boundary: plant + I/O module live in a separate io_node process."""
    io_mode = 'remote'

    def io(self):
        return ModbusClient(port=self.io_port, timeout=.5)

    def restart_io_node(self):
        stop(type(self).io_process)
        type(self).io_process = self.start_io_node()
        self.assertTrue(wait_for(lambda: self.modbus.read(4, 21, 1)[0] == 1, timeout=6), 'I/O link not restored')

    def test_01_flow_comes_from_remote_node(self):
        self.start_running()
        regs = self.modbus.read(4, 20, 3)
        self.assertEqual(regs[0], 1, 'IO_MODE remote')
        self.assertEqual(regs[1], 1, 'I/O healthy')
        self.assertLess(regs[2], 300, 'sample age below stale timeout')
        node = self.io()
        try:
            self.assertTrue(wait_for(lambda: node.read(4, 4, 1)[0] == 1), 'pump command reached the I/O node')
            self.assertGreater(node.read(4, 1, 1)[0], 50, 'remote flow meter reports flow')
        finally:
            node.close()

    def test_02_io_node_crash_trips_and_recovers(self):
        self.start_running()
        stop(type(self).io_process)
        self.assertTrue(wait_for(lambda: self.current_state()[5] == 6, timeout=3), 'IO_COMM_LOSS expected')
        self.assertEqual(self.current_state()[4], 0, 'pump de-energized')
        self.gateway.command('reset')
        time.sleep(.2)
        self.assertEqual(self.current_state()[0], 3, 'reset refused while I/O is down')
        self.restart_io_node()
        self.gateway.command('reset')
        self.assertTrue(wait_for(lambda: self.current_state()[0] == 0))

    def test_03_frozen_samples_are_detected_as_stale(self):
        self.start_running()
        node = self.io()
        try:
            node.write(7, 1)  # BENCH_FREEZE: transmitter stuck, link still up
            self.assertTrue(wait_for(lambda: self.current_state()[5] == 6, timeout=3))
            node.write(7, 0)
        finally:
            node.close()

    def test_04_network_latency_tolerated_then_trips(self):
        self.start_running()
        bench = ModbusClient(port=self.io_port, timeout=3)  # bench writes are themselves delayed
        try:
            bench.write(8, 60)  # 60 ms per response: still inside the 300 ms stale budget
            time.sleep(1.5)
            self.assertEqual(self.current_state()[0], 2, 'moderate latency must not trip')
            bench.write(8, 500)  # beyond the controller's I/O timeout
            self.assertTrue(wait_for(lambda: self.current_state()[5] == 6, timeout=4))
        finally:
            bench.write(8, 0)
            bench.close()

    def test_05_low_flow_fault_injected_at_the_remote_node(self):
        self.start_running()
        self.gateway.command('low_flow', True)
        self.assertTrue(wait_for(lambda: self.current_state()[5] == 1))
        node = self.io()
        try:
            self.assertEqual(node.read(3, 3, 1)[0], 1, 'injection forwarded to the I/O node')
        finally:
            node.close()

    def test_99_controller_crash_drives_io_node_to_failsafe(self):
        self.start_running()
        node = self.io()
        try:
            self.assertEqual(node.read(4, 4, 1)[0], 1)
            stop(type(self).process)
            self.assertTrue(wait_for(lambda: node.read(4, 6, 1)[0] == 1, timeout=2), 'output watchdog fail-safe')
            self.assertEqual(node.read(4, 4, 1)[0], 0, 'pump contactor released by the I/O node')
        finally:
            node.close()


if __name__ == '__main__': unittest.main()
