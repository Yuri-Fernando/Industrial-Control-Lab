"""Modbus TCP client for FC03/04/06/16 with MBAP transaction validation. No pip dependencies.

The TCP connection is persistent (as SCADA masters do) and transparently re-established after any
transport or framing error; a Modbus exception response keeps the link open.
"""
import socket
import struct
import threading


class ModbusError(Exception):
    """Transport, protocol, or Modbus exception."""


class ModbusClient:
    def __init__(self, host='127.0.0.1', port=15020, unit=1, timeout=2):
        self.host, self.port, self.unit = host, int(port), unit
        self.timeout = timeout
        self._lock = threading.RLock()
        self._transaction = 0
        self._sock = None

    def close(self):
        with self._lock:
            if self._sock is not None:
                try:
                    self._sock.close()
                finally:
                    self._sock = None

    def _connection(self):
        if self._sock is None:
            sock = socket.create_connection((self.host, self.port), self.timeout)
            sock.settimeout(self.timeout)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._sock = sock
        return self._sock

    def _exchange(self, function, payload):
        with self._lock:
            self._transaction = (self._transaction + 1) & 0xffff
            txn = self._transaction
            pdu = bytes([function]) + payload
            request = struct.pack('>HHHB', txn, 0, len(pdu) + 1, self.unit) + pdu
            try:
                sock = self._connection()
                sock.sendall(request)
                header = self._read_exact(sock, 7)
                recv_txn, protocol, length, unit = struct.unpack('>HHHB', header)
                if recv_txn != txn or protocol != 0 or unit != self.unit or not (2 <= length <= 254):
                    raise ModbusError('Invalid MBAP response')
                response = self._read_exact(sock, length - 1)
            except ModbusError:
                self.close()
                raise
            except (OSError, EOFError) as exc:
                self.close()
                raise ModbusError(str(exc)) from exc
            if response[0] == (function | 0x80):
                raise ModbusError(f'Modbus exception {response[1]}')
            if response[0] != function:
                self.close()
                raise ModbusError('Unexpected function code')
            return response[1:]

    @staticmethod
    def _read_exact(sock, length):
        buf = bytearray()
        while len(buf) < length:
            chunk = sock.recv(length - len(buf))
            if not chunk:
                raise EOFError('Socket disconnected')
            buf.extend(chunk)
        return bytes(buf)

    def read(self, function, address, quantity):
        if function not in (3, 4) or not (1 <= quantity <= 125):
            raise ValueError('Invalid read request')
        payload = self._exchange(function, struct.pack('>HH', address, quantity))
        if len(payload) != quantity * 2 + 1 or payload[0] != quantity * 2:
            raise ModbusError('Incorrect response length')
        return struct.unpack('>' + 'H' * quantity, payload[1:])

    def write(self, address, value):
        if not 0 <= value <= 65535:
            raise ValueError('Not a uint16')
        raw = struct.pack('>HH', address, value)
        if self._exchange(6, raw) != raw:
            raise ModbusError('Write echo mismatch')

    def write_multiple(self, address, values):
        if not 1 <= len(values) <= 123 or any(not 0 <= v <= 65535 for v in values):
            raise ValueError('Invalid FC16 request')
        body = struct.pack('>HHB', address, len(values), 2 * len(values)) + struct.pack('>' + 'H' * len(values), *values)
        if self._exchange(16, body) != struct.pack('>HH', address, len(values)):
            raise ModbusError('FC16 echo mismatch')


INPUT_REGISTERS = 28  # controller input image size read by the gateway (offsets 0..27)
STATES = ['STOPPED', 'STARTING', 'RUNNING', 'FAULT', 'ESTOP']
FAULTS = ['NONE', 'LOW_FLOW', 'OVERTEMP', 'SENSOR_INVALID', 'WATCHDOG', 'EMERGENCY_STOP', 'IO_COMM_LOSS']


def decode(holding, inputs):
    state, fault = inputs[0], inputs[5]
    signed_error = inputs[12] if inputs[12] < 32768 else inputs[12] - 65536
    sample = {
        'state': STATES[state] if state < len(STATES) else 'UNKNOWN',
        'fault': FAULTS[fault] if fault < len(FAULTS) else 'UNKNOWN',
        'temperature_c': inputs[1]/10,
        'setpoint_c': holding[1]/10,
        'flow_lpm': inputs[2]/10,
        'cooling_pct': inputs[3]/10,
        'pump': bool(inputs[4]),
        'heat_load_pct': holding[2],
        'inject_low_flow': bool(holding[3]),
        'inject_sensor_failure': bool(holding[4]),
        'inject_overtemperature': bool(holding[6]),
        'sensor_valid': bool(inputs[13]),
        'control_error_c': signed_error/10,
        'max_scheduler_jitter_us': inputs[6],
        'missed_deadlines': inputs[7],
        'uptime_s': inputs[8],
        'watchdog_age_ms': inputs[9],
        'safety_trip_count': inputs[11],
        'quality': 'GOOD',
    }
    if len(inputs) >= INPUT_REGISTERS:
        sample.update({
            'scan_lateness_p50_us': inputs[14],
            'scan_lateness_p95_us': inputs[15],
            'scan_lateness_p99_us': inputs[16],
            'scan_lateness_window_max_us': inputs[17],
            'scan_exec_p99_us': inputs[18],
            'scan_exec_max_us': inputs[19],
            'io_mode': 'remote' if inputs[20] else 'local',
            'io_healthy': bool(inputs[21]),
            'io_sample_age_ms': inputs[22],
            'io_comm_errors': inputs[23],
            'control_engine': 'st' if inputs[24] else 'cpp',
            'rt_elevated': bool(inputs[25]),
            'engine_mismatches': inputs[26],
            'shadow_engine': ('cpp' if inputs[24] else 'st') if inputs[27] else 'none',
        })
    return sample
