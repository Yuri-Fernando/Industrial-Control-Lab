#!/usr/bin/env python3
"""Industrial Control Lab — one cross-platform entry point (Windows, Linux, WSL). Stdlib only.

  python scripts/lab.py build                       compile controller + remote I/O node (PlatformIO, else g++)
  python scripts/lab.py run [--io remote] [--engine st] [--rt] [--no-browser]
                                                    start the stack and open the HMI at http://127.0.0.1:8088
  python scripts/lab.py test                        all gates: PlatformIO Unity + Python integration
  python scripts/lab.py demo                        REST acceptance campaign against a running stack
  python scripts/lab.py bench [--seconds 15]        scan-timing characterization (idle vs CPU stress, RT on/off)
"""
import argparse
import json
import multiprocessing
import os
import platform
import shutil
import subprocess
import sys
import time
import webbrowser
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tests_python.binaries import controller_binary, io_node_binary  # noqa: E402

WINDOWS = sys.platform.startswith('win')
EXE = '.exe' if WINDOWS else ''
SOURCES_COMMON = ['lib/control/control.cpp', 'lib/modbus/modbus.cpp', 'lib/net/net.cpp', 'lib/io/io.cpp']


def find_pio():
    found = shutil.which('pio') or shutil.which('platformio')
    if found:
        return found
    home = Path.home() / '.platformio' / 'penv' / ('Scripts' if WINDOWS else 'bin') / f'pio{EXE}'
    return str(home) if home.is_file() else None


def run(cmd, **kw):
    print('$', ' '.join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], cwd=ROOT, **kw).returncode


def build_gcc():
    gxx = shutil.which('g++')
    if not gxx:
        sys.exit('No PlatformIO and no g++ on PATH. Install PlatformIO IDE or a GCC toolchain (see README).')
    (ROOT / 'build').mkdir(exist_ok=True)
    flags = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-pthread'] + \
        [f'-Ilib/{d}' for d in ('control', 'modbus', 'net', 'io', 'stvm', 'timing')]
    libs = ['-lws2_32', '-lwinmm', '-static-libgcc', '-static-libstdc++'] if WINDOWS else []
    rc = run([gxx, *flags, 'src/main.cpp', *SOURCES_COMMON, 'lib/stvm/stvm.cpp', 'lib/stvm/st_controller.cpp',
              'lib/timing/cycle_stats.cpp', '-o', f'build/industrial_controller{EXE}', *libs])
    rc = rc or run([gxx, *flags, 'src/io_node/main.cpp', *SOURCES_COMMON, '-o', f'build/io_node{EXE}', *libs])
    if rc == 0 and WINDOWS:  # runtime DLL when the toolchain has no static winpthread
        dll = Path(gxx).parent / 'libwinpthread-1.dll'
        if dll.is_file():
            shutil.copy2(dll, ROOT / 'build' / dll.name)
    return rc


def cmd_build(_args):
    pio = find_pio()
    rc = run([pio, 'run']) if pio else build_gcc()
    if rc:
        sys.exit(rc)


def ensure_built():
    try:
        controller_binary(); io_node_binary()
    except RuntimeError:
        cmd_build(None)


def spawn(cmd, env, quiet=False):
    out = subprocess.DEVNULL if quiet else None
    proc = subprocess.Popen([str(c) for c in cmd], cwd=ROOT, env={**os.environ, **env}, stdout=out, stderr=out,
                            preexec_fn=None if WINDOWS else _die_with_parent)
    if WINDOWS:
        _kill_on_close_job().assign(proc)
    return proc


def _die_with_parent():  # Linux: child receives SIGTERM if lab.py dies, even when killed with SIGKILL
    import ctypes
    import signal
    try:
        ctypes.CDLL('libc.so.6', use_errno=True).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    except OSError:
        pass


class _Job:
    """Windows Job Object with KILL_ON_JOB_CLOSE: closing lab.py (even forcibly) terminates the whole stack."""

    def __init__(self):
        import ctypes
        from ctypes import wintypes
        self.k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        self.k32.CreateJobObjectW.restype = wintypes.HANDLE
        self.k32.CreateJobObjectW.argtypes = [wintypes.LPVOID, wintypes.LPCWSTR]
        self.k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
        self.k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self.handle = self.k32.CreateJobObjectW(None, None)

        class Limits(ctypes.Structure):  # JOBOBJECT_EXTENDED_LIMIT_INFORMATION (x64 layout)
            _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                        ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                        ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                        ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD),
                        ('SchedulingClass', wintypes.DWORD), ('IoCounters', ctypes.c_uint64 * 6),
                        ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                        ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]
        limits = Limits()
        limits.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        self.k32.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits))

    def assign(self, proc):
        if self.handle:
            self.k32.AssignProcessToJobObject(self.handle, int(proc._handle))


_JOB = None


def _kill_on_close_job():
    global _JOB
    if _JOB is None:
        _JOB = _Job()
    return _JOB


def terminate(procs):
    for p in reversed(procs):
        if p.poll() is None:
            p.terminate()
    for p in procs:
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            p.kill()


def wait_http(url, timeout=10):
    from urllib.request import urlopen
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            with urlopen(url, timeout=1) as r:
                if r.status == 200:
                    return True
        except OSError:
            time.sleep(.3)
    return False


def start_stack(args, quiet=False):
    ensure_built()
    procs = []
    env = {'CONTROL_BIND': '127.0.0.1', 'CONTROL_PORT': str(args.control_port), 'IO_MODE': args.io,
           'IO_PORT': str(args.io_port), 'CONTROL_ENGINE': args.engine, 'ICL_RT': '1' if args.rt else '0'}
    if args.io == 'remote':
        procs.append(spawn([io_node_binary()], {'IO_BIND': '127.0.0.1', 'IO_PORT': str(args.io_port)}, quiet))
        time.sleep(.3)
    procs.append(spawn([controller_binary()], env, quiet))
    gw_env = {'CONTROL_HOST': '127.0.0.1', 'CONTROL_PORT': str(args.control_port), 'HTTP_BIND': '127.0.0.1',
              'HTTP_PORT': str(args.http_port), 'MQTT_HOST': args.mqtt_host}
    procs.append(spawn([sys.executable, '-m', 'gateway.server'], gw_env, quiet))
    return procs


def cmd_run(args):
    procs = start_stack(args)
    url = f'http://127.0.0.1:{args.http_port}'
    try:
        if not wait_http(url + '/health'):
            print('WARNING: gateway did not report a healthy controller yet; check the logs above.')
        print(f'\n  HMI:     {url}\n  REST:    {url}/api/status\n  Metrics: {url}/metrics\n'
              f'  Modbus:  controller 127.0.0.1:{args.control_port}'
              + (f', remote I/O node 127.0.0.1:{args.io_port}' if args.io == 'remote' else '')
              + f'\n  Engine:  {args.engine}   I/O: {args.io}   RT tuning: {args.rt}\n\nCtrl+C to stop.\n', flush=True)
        if not args.no_browser:
            webbrowser.open(url)
        while all(p.poll() is None for p in procs):
            time.sleep(.5)
        print('A process exited; stopping the stack.')
    except KeyboardInterrupt:
        pass
    finally:
        terminate(procs)


def cmd_test(_args):
    ensure_built()
    pio = find_pio()
    rc = 0
    if pio:
        rc |= run([pio, 'run'])
        rc |= run([pio, 'test', '-e', 'test'])
    else:
        print('NOTE: PlatformIO not found; skipping Unity tests (gcc build used).')
    rc |= run([sys.executable, '-m', 'compileall', '-q', 'gateway', 'scripts', 'tests_python'])
    rc |= run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests_python', '-v'])
    print('\nALL GATES PASSED' if rc == 0 else '\nGATES FAILED')
    sys.exit(rc)


def cmd_demo(args):
    sys.exit(run([sys.executable, 'scripts/demo_scenarios.py', '--host', f'http://127.0.0.1:{args.http_port}']))


def _burn(stop_at):
    x = 0
    while time.monotonic() < stop_at:
        x = (x * 1103515245 + 12345) & 0xffffffff


def cmd_bench(args):
    """Measure scan lateness/exec percentiles from the controller's own registers under several conditions."""
    from gateway.modbus_client import ModbusClient
    ensure_built()
    results = []
    stressors = multiprocessing.cpu_count()
    for stress in (False, True):
        for rt in (False, True):
            port = args.control_port + 10
            env = {'CONTROL_BIND': '127.0.0.1', 'CONTROL_PORT': str(port), 'ICL_RT': '1' if rt else '0'}
            proc = spawn([controller_binary()], env, quiet=True)
            burners = []
            try:
                time.sleep(1.0)
                if stress:
                    stop_at = time.monotonic() + args.seconds + 2
                    burners = [multiprocessing.Process(target=_burn, args=(stop_at,)) for _ in range(stressors)]
                    for b in burners:
                        b.start()
                client = ModbusClient(port=port, timeout=2)
                deadline = time.monotonic() + args.seconds
                while time.monotonic() < deadline:  # keep the heartbeat alive like a gateway would
                    client.write(5, int(time.monotonic() * 10) & 0xffff)
                    time.sleep(.5)
                r = client.read(4, 0, 28)
                client.close()
                row = {'cpu_stress_processes': stressors if stress else 0, 'rt_tuning_requested': rt,
                       'rt_elevated': bool(r[25]), 'window_s': args.seconds,
                       'lateness_p50_us': r[14], 'lateness_p95_us': r[15], 'lateness_p99_us': r[16],
                       'lateness_max_us': r[17], 'exec_p99_us': r[18], 'exec_max_us': r[19],
                       'missed_deadlines': r[7]}
                results.append(row)
                print(json.dumps(row), flush=True)
            finally:
                for b in burners:
                    b.join(timeout=5)
                terminate([proc])
    report = {'name': 'scan timing characterization (50 ms period)',
              'timestamp_utc': datetime.now(timezone.utc).isoformat(),
              'host': {'os': platform.platform(), 'python': platform.python_version(),
                       'cpus': multiprocessing.cpu_count()},
              'note': 'Soft real-time measurement on a general-purpose OS; not a hard real-time guarantee.',
              'results': results}
    out = ROOT / 'reports' / f'timing_{platform.system().lower()}.json'
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('Saved:', out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('build')
    p_run = sub.add_parser('run')
    p_run.add_argument('--io', choices=['local', 'remote'], default='local')
    p_run.add_argument('--engine', choices=['cpp', 'st'], default='cpp')
    p_run.add_argument('--rt', action='store_true', help='request real-time scheduling for the scan thread')
    p_run.add_argument('--no-browser', action='store_true')
    p_run.add_argument('--mqtt-host', default=os.getenv('MQTT_HOST', ''))
    sub.add_parser('test')
    sub.add_parser('demo')
    p_bench = sub.add_parser('bench')
    p_bench.add_argument('--seconds', type=int, default=15)
    for p in (p_run, sub.choices['demo'], p_bench):
        p.add_argument('--http-port', type=int, default=8088)
        p.add_argument('--control-port', type=int, default=15020)
        p.add_argument('--io-port', type=int, default=15021)
    args = ap.parse_args()
    {'build': cmd_build, 'run': cmd_run, 'test': cmd_test, 'demo': cmd_demo, 'bench': cmd_bench}[args.cmd](args)


if __name__ == '__main__':
    main()
