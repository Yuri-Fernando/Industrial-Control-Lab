#!/usr/bin/env python3
"""Small REST-based acceptance campaign; requires controller+gateway running."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sys
import time
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
HOST = sys.argv[sys.argv.index('--host') + 1] if '--host' in sys.argv else os.getenv('ICL_HMI', 'http://127.0.0.1:8088')

def get_status():
    with urlopen(HOST+'/api/status',timeout=2) as r: return json.load(r)

def command(name, value=None):
    req = Request(HOST+'/api/command', data=json.dumps({'command':name,'value':value}).encode(),
                  headers={'Content-Type':'application/json'},method='POST')
    with urlopen(req,timeout=2) as r: return json.load(r)

def wait_state(target, timeout=5):
    end=time.monotonic()+timeout
    while time.monotonic() < end:
        v=get_status()
        if v.get('state')==target: return v
        time.sleep(.2)
    raise AssertionError(f'Expected {target}, got {get_status()}')

def scenario():
    results=[]
    def check(name, fn):
        try:
            result=fn()
            results.append({'scenario':name,'result':'PASS','sample':result})
            print('PASS',name)
        except Exception as e:
            results.append({'scenario':name,'result':'FAIL','reason':str(e)})
            print('FAIL',name,e)

    def baseline():
        for fault in ('low_flow','sensor_failure','overtemperature'): command(fault,False)
        command('stop'); time.sleep(.2); command('reset')
        return wait_state('STOPPED')
    check('baseline', baseline)
    def startup():
        command('start'); return wait_state('RUNNING')
    check('start_and_flow',startup)
    def flow_trip():
        command('low_flow',True)
        sample=wait_state('FAULT')
        assert sample['fault']=='LOW_FLOW',sample
        assert sample['pump'] is False,sample
        command('stop'); time.sleep(.3)
        assert get_status()['state']=='FAULT'
        command('low_flow',False); command('reset'); wait_state('STOPPED')
        return sample
    check('flow_trip_is_latched',flow_trip)
    def estop():
        command('start'); wait_state('RUNNING')
        command('estop'); return wait_state('ESTOP')
    check('estop_simulation',estop)
    def sensor_trip():
        baseline(); command('start'); wait_state('RUNNING')
        command('sensor_failure',True)
        sample=wait_state('FAULT')
        assert sample['fault']=='SENSOR_INVALID',sample
        command('sensor_failure',False)
        return sample
    check('sensor_fault_trip',sensor_trip)
    check('cleanup',baseline)
    final=get_status()
    report={'name':'Industrial Control Lab acceptance demo',
            'timestamp_utc':datetime.now(timezone.utc).isoformat(),
            'configuration':{k:final.get(k) for k in ('control_engine','shadow_engine','io_mode','engine_mismatches')},
            'pass':sum(item['result']=='PASS' for item in results),
            'total':len(results),'results':results}
    (ROOT/'reports').mkdir(exist_ok=True)
    dest=ROOT/'reports'/'acceptance_latest.json'
    dest.write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('Saved:',dest)
    if report['pass']!=report['total']: raise SystemExit(1)

if __name__=='__main__': scenario()
