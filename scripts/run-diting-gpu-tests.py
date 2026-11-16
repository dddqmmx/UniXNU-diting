#!/usr/bin/env python3
"""Run fixed GPU/colour/UI workloads and assess them through one USB session.

Exit 0: complete selected GPU checks passed; 2: test failure/incomplete physical
performance evidence; 3: transport/infrastructure failure. Device is left running.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import time
from diting_usb_ports import serial_ports
from diting_usb_session import DitingUsbSession
from diting_gpu_automation import Collector,PROFILES,POLICIES,STREAM,assess,correlate_trace,status,write_reports


def run(session, output, profile, policies, timeout=8):
    collector=Collector(session,output,timeout);error=None
    started=datetime.now(timezone.utc).isoformat()
    try:
        ready=status(collector.command('gputest status'),'gputest status')
        # Bootstrap/launchd readiness is a prerequisite, not a benchmark.
        deadline=time.monotonic()+90
        while not ready['daemon'] or ready['heartbeat_age_us']>=5000000:
            if time.monotonic()>=deadline:raise TimeoutError('GPU daemon readiness timeout')
            time.sleep(.5);ready=status(collector.command('gputest status'),'gputest status')
        response=collector.command('status');(Path(output)/'before-status.raw.log').write_bytes(response)
        response=collector.command('gpu');(Path(output)/'before-gpu.raw.log').write_bytes(response)
        # Anchor at the current producer watermark. Historical ring data is
        # intentionally excluded from the automatic workload's coverage.
        initial=collector.command('trace 0 1');chunk,_=STREAM.decode_chunk(initial,0,1)
        collector.cursor=chunk['issued'];collector.dropped=collector.initial_dropped=chunk['dropped']
        collector.sample_display()
        for policy in policies:
            print(f'GPU automatic test: {policy}/{profile}',flush=True)
            collector.run_job(profile,policy)
        collector.drain_trace()
    except BaseException as failure:
        error=f'{type(failure).__name__}: {failure}'
    finally:
        collector.close()
    report=collector.report()
    report.update(schema=1,started_utc=started,finished_utc=datetime.now(timezone.utc).isoformat(),
                  target_fps=120,manual_interaction_required=False,execution='ERROR' if error else 'COMPLETE')
    if error:report['infrastructure_error']=error
    try:
        correlate_trace(output,report)
    except Exception as failure:
        error=error or f'{type(failure).__name__}: {failure}'
        report['infrastructure_error']=error
        report['execution']='ERROR'
    assess(report)
    write_reports(output,report)
    print(json.dumps({key:report[key] for key in ('assessment','execution','target_fps','hardware_performance_pass')},ensure_ascii=False),flush=True)
    return 3 if error else 0 if report['assessment']=='PASS' else 2


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port')
    parser.add_argument('--profile',choices=PROFILES,default='all')
    parser.add_argument('--policies',choices=POLICIES,nargs='+',default=['auto','sysmem','gmem'])
    parser.add_argument('--command-timeout',type=float,default=8)
    parser.add_argument('--output-dir',type=Path,required=True)
    args=parser.parse_args()
    if len(args.policies)!=len(set(args.policies)):parser.error('each policy may run once per invocation')
    if not 0<args.command_timeout<=30:parser.error('command timeout must be 0..30 seconds')
    if args.profile=='ui' and args.policies!=['auto']:parser.error('UI policy is controlled by the running render server; use --policies auto')
    ports=[args.port] if args.port else serial_ports()
    if len(ports)!=1:parser.error('expected one Diting CDC port; use --port')
    args.output_dir.mkdir(parents=True,exist_ok=False)
    try:
        with DitingUsbSession(ports[0]) as session:
            return run(session,args.output_dir,args.profile,args.policies,args.command_timeout)
    except Exception as error:
        report=assess(dict(schema=1,jobs=[],execution='ERROR',manual_interaction_required=False,
                           infrastructure_error=f'{type(error).__name__}: {error}',hardware_performance_pass=False))
        write_reports(args.output_dir,report);print(report['infrastructure_error']);return 3


if __name__=='__main__':
    raise SystemExit(main())
