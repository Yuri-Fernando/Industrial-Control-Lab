"""REST supervision + MQTT outbound telemetry for a virtual industrial loop.

For isolated lab use only. No production authentication, TLS, or physical interlocks.
"""
import json
import logging
import os
from pathlib import Path
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

from collections import deque

from .modbus_client import INPUT_REGISTERS, ModbusClient, ModbusError, decode

HISTORY_FIELDS = ('timestamp_utc', 'state', 'temperature_c', 'setpoint_c', 'flow_lpm', 'cooling_pct')

try:
    import paho.mqtt.client as mqtt
except ImportError:
    mqtt = None

logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s')
log = logging.getLogger(__name__)
ROOT = Path(__file__).resolve().parent


class Gateway:
    def __init__(self, host=None, port=None, mqtt_host=None):
        self.client = ModbusClient(host or os.getenv('CONTROL_HOST', '127.0.0.1'),
                                   port or int(os.getenv('CONTROL_PORT', '15020')))
        self.mqtt_host = mqtt_host if mqtt_host is not None else os.getenv('MQTT_HOST', '')
        self.mqtt_port = int(os.getenv('MQTT_PORT', '1883'))
        self.mqtt_client = None
        self.last_sample = {'quality': 'DISCONNECTED', 'error': 'No telemetry yet'}
        self._guard = threading.Lock()
        self._stop = threading.Event()
        self._worker = None
        self._heartbeat = 0
        self._last_fault = None
        self._history = deque(maxlen=240)  # ~2 min of 0.5 s samples for the HMI trend

    def start(self):
        if self.mqtt_host and mqtt:
            try:
                self.mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                               client_id='icl-gateway')
                # Last-will: subscribers learn the edge gateway died even without a clean disconnect.
                self.mqtt_client.will_set('icl/thermal/gateway/status', 'OFFLINE', qos=1, retain=True)
                self.mqtt_client.on_connect = lambda c, *_: c.publish(
                    'icl/thermal/gateway/status', 'ONLINE', qos=1, retain=True)
                self.mqtt_client.connect_async(self.mqtt_host, self.mqtt_port, 10)
                self.mqtt_client.loop_start()
            except (OSError, ValueError) as exc:
                log.warning('MQTT broker unavailable: %s', exc)
                self.mqtt_client = None
        elif self.mqtt_host and not mqtt:
            log.warning('paho-mqtt not installed; continuing without MQTT')
        self._worker = threading.Thread(target=self._poll, daemon=True, name='gateway-poll')
        self._worker.start()

    def close(self):
        self._stop.set()
        if self._worker: self._worker.join(timeout=2)
        self.client.close()
        if self.mqtt_client:
            self.mqtt_client.publish('icl/thermal/gateway/status', 'OFFLINE', qos=1, retain=True)
            self.mqtt_client.loop_stop()
            self.mqtt_client.disconnect()

    def _poll(self):
        while not self._stop.is_set():
            try:
                self._heartbeat = (self._heartbeat + 1) & 0xffff
                self.client.write(5, self._heartbeat)
                holding = self.client.read(3, 0, 7)
                inputs = self.client.read(4, 0, INPUT_REGISTERS)
                sample = decode(holding, inputs)
                sample['timestamp_utc'] = datetime.now(timezone.utc).isoformat()
                with self._guard:
                    self.last_sample = sample
                    self._history.append({k: sample[k] for k in HISTORY_FIELDS})
                if self.mqtt_client and self.mqtt_client.is_connected():
                    self.mqtt_client.publish('icl/thermal/telemetry', json.dumps(sample), qos=0, retain=False)
                    self.mqtt_client.publish('icl/thermal/state', sample['state'], qos=1, retain=True)
                    if sample['fault'] != self._last_fault:  # alarm events only on change (edge-triggered)
                        self.mqtt_client.publish('icl/thermal/alarm', json.dumps(
                            {'fault': sample['fault'], 'state': sample['state'],
                             'timestamp_utc': sample['timestamp_utc']}), qos=1, retain=True)
                self._last_fault = sample['fault']
            except (ModbusError, OSError, ValueError) as exc:
                with self._guard:
                    self.last_sample = {'quality': 'DISCONNECTED', 'error': str(exc),
                                        'timestamp_utc': datetime.now(timezone.utc).isoformat()}
                log.warning('Modbus poll failed: %s', exc)
            self._stop.wait(0.5)

    def history(self):
        with self._guard: return list(self._history)

    def status(self):
        with self._guard: return dict(self.last_sample)

    def command(self, name, value=None):
        commands = {'start': 1, 'stop': 2, 'reset': 3, 'estop': 4}
        if name in commands:
            self.client.write(0, commands[name])
        elif name == 'setpoint':
            value = float(value)
            if not 24 <= value <= 38: raise ValueError('setpoint must be in [24, 38] C')
            self.client.write(1, round(value*10))
        elif name == 'heat_load':
            value = int(value)
            if not 0 <= value <= 100: raise ValueError('heat_load must be 0..100')
            self.client.write(2, value)
        elif name in ('low_flow', 'sensor_failure', 'overtemperature'):
            if type(value) is not bool: raise ValueError('Fault injection must be boolean')
            self.client.write({'low_flow': 3, 'sensor_failure': 4, 'overtemperature': 6}[name], int(value))
        else:
            raise ValueError('Unknown command')
        return {'ok': True, 'command': name}


def make_handler(gateway):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, data, status=200, content_type='application/json'):
            body = data if isinstance(data, bytes) else json.dumps(data).encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == '/':
                self._send((ROOT/'static'/'index.html').read_bytes(), content_type='text/html; charset=utf-8')
            elif path == '/api/history':
                self._send(gateway.history())
            elif path == '/api/status':
                self._send(gateway.status())
            elif path == '/health':
                state = gateway.status()
                self._send({'gateway': 'up', 'controller': state.get('quality', 'DISCONNECTED')},
                           status=200 if state.get('quality') == 'GOOD' else 503)
            elif path == '/metrics':
                state = gateway.status()
                lines = ['# TYPE icl_controller_connected gauge',
                         f'icl_controller_connected {int(state.get("quality") == "GOOD")}']
                for field in ('temperature_c','flow_lpm','cooling_pct','missed_deadlines',
                              'max_scheduler_jitter_us','safety_trip_count','watchdog_age_ms',
                              'scan_lateness_p50_us','scan_lateness_p95_us','scan_lateness_p99_us',
                              'scan_exec_p99_us','io_sample_age_ms','io_comm_errors','engine_mismatches'):
                    if field in state:
                        lines.extend([f'# TYPE icl_{field} gauge',f'icl_{field} {state[field]}'])
                if 'io_healthy' in state:
                    lines.extend(['# TYPE icl_io_healthy gauge', f'icl_io_healthy {int(state["io_healthy"])}'])
                self._send(('\n'.join(lines)+'\n').encode(), content_type='text/plain; version=0.0.4')
            else:
                self._send({'error': 'not found'}, 404)

        def do_POST(self):
            if urlsplit(self.path).path != '/api/command':
                return self._send({'error': 'not found'}, 404)
            try:
                length = int(self.headers.get('Content-Length','0'))
                if not 0 < length <= 4096: raise ValueError('invalid request size')
                payload = json.loads(self.rfile.read(length))
                result = gateway.command(payload['command'], payload.get('value'))
                self._send(result)
            except (ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
                self._send({'error': str(exc)}, 400)
            except ModbusError as exc:
                self._send({'error': str(exc)}, 503)

        def log_message(self, fmt, *args):
            log.info('%s - %s', self.client_address[0], fmt % args)
    return Handler


def main():
    gateway = Gateway()
    gateway.start()
    host = os.getenv('HTTP_BIND', '127.0.0.1')
    port = int(os.getenv('HTTP_PORT', '8088'))
    server = ThreadingHTTPServer((host, port), make_handler(gateway))
    log.info('Supervision UI: http://%s:%s', host, port)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: server.server_close(); gateway.close()

if __name__ == '__main__': main()
