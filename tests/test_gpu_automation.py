"""Real broker/process fixtures, independent result oracles and fragmented PTY USB."""
import base64
import copy
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import tempfile
import termios
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import diting_gpu_automation as gpu
from diting_usb_session import DitingUsbSession
from diting_gpu_test_build import shaders,metallib

def import_script(name,filename):
    spec=importlib.util.spec_from_file_location(name,ROOT/'scripts'/filename)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
runner=import_script('owned_gpu_runner','run-diting-gpu-tests.py')
cli=import_script('owned_gpu_usb_cli','diting-usb-command.py')

def frame(command,payload):
    if isinstance(payload,str):payload=payload.encode()
    return ('DitingUSB BEGIN '+command+'\n').encode()+payload+('DitingUSB END '+command+'\n').encode()

def fixture(profile=1,policy=0,job=1):
    manifest=dict(kind='manifest',schema=1,job=job,profile=profile,policy=policy,pid=31,host_supplied_shader=False,
                  **{key:gpu.CATALOG[key] for key in ('width','height','frames','warmup','target_fps')})
    rows=[manifest];start=1000000000
    for spec in gpu.expected_cases(profile,policy).values():
        case_id=spec['id'];record=dict(kind='case',id=case_id,name=spec['name'],category=spec['category'],required=True,
                                     pass_=True,start_ns=start)
        record['pass']=record.pop('pass_')
        if spec['category']=='colour':record.update(pixels=257*129,mismatches=0,guard_errors=0,max_channel_error=0,first_mismatches=[])
        if case_id in (15,16,17,18,19,31,32,33,34,35,36,37,46,47,48):
            partial=case_id in (34,35,36,37,46,47,48);surface=case_id in (31,32,33);imported=case_id in (46,47,48)
            minimum=7 if partial else 2 if case_id in (15,16,19) else 4 if surface else 1
            modes=[0]*8;modes[0 if policy==2 else 4]=minimum
            sides=[0]*16;events=[0]*8
            if policy==2:
                if case_id in (17,18):sides[12]=1
                if case_id in (15,16,19):events[0]=events[3]=1;events[4]=events[7]=2
                if case_id==17:sides[15]=1
                if surface:events[0]=events[4]=minimum
                if partial:
                    sides[8]=6
                    if case_id!=37:sides[11]=6
                    if imported:sides[4]=sides[5]=sides[7]=6
                    else:
                        events[0]=6;events[4]=1
                        if case_id!=37:events[3]=6;events[7]=1
            record.update(path_verified=True,linear_tiling=None if surface else case_id not in (18,37),
                          render_target_kind='metal-iosurface' if surface else 'vulkan-optimal-image' if case_id in (18,37) else 'vulkan-linear-image',
                          linear_transfer_observed=bool(sum(sides[3::4])+sum(events[3::4])),input_fetch=case_id in (17,18),
                          minimum_render_passes=minimum,render_mode_delta=modes,gmem_side_delta=sides,gmem_event_delta=events)
            if case_id in (15,16,19) or partial:
                record['seed_pixels']={
                    'pass':True,'pixels':257*129,'mismatches':0,'guard_errors':0,'max_channel_error':0,'first_mismatches':[]}
            if surface or partial:
                pixel_check=dict(pass_=True,pixels=257*129,mismatches=0,guard_errors=0,max_channel_error=0,first_mismatches=[])
                pixel_check['pass']=pixel_check.pop('pass_')
                record.update(reuse_cycles=6 if partial else 4,
                              cycle_pixels=[copy.deepcopy(pixel_check) for _ in range(6 if partial else 4)],
                              readback_source=('original imported host pointer plus independent transfer-buffer oracle' if imported else
                                               'raw mapped linear image plus independent transfer-buffer oracle' if case_id!=37 else 'Vulkan transfer buffer')
                                  if partial else 'IOSurface CPU mapping after GPU completion and IOSurfaceLock')
                if partial:
                    destinations=[0]*12
                    if policy==2:
                        destinations[0]=6
                        if imported:destinations[8]=6
                    record.update(partial_render_area=True,draw_transfer_verified=True,gmem_destination_delta=destinations,
                                  imported_host_pointer=imported,draw_load_required=imported,
                                  linear_storage_pixels=[copy.deepcopy(pixel_check) for _ in range(0 if case_id==37 else 7)])
                    if imported:record.update(external_handle_type=0x80,host_pointer_alignment=4096,host_allocation_bytes=163840)
        if spec['category']=='synchronization':record.update(words=4096,mismatches=0)
        if spec['category']=='lifetime':record.update(cycles=64,mismatches=0)
        if spec['category']=='ui':
            record.update(content_hashes=[1,2,3],distinct_content=3,minimum_distinct_content=2 if case_id==62 else 3,
                          accepted_events={60:354,61:92,62:50}[case_id],refused_events=0,foreground_verified=True)
            count={60:8,61:2,62:1}[case_id]
            frames=[(i*8333,50,i+1) for i in range(count*43)]
            record.update(frame_sample_start_ns=start+1000,frame_sample_hz=120,
                          frame_sample_format='le-u32-elapsed-us,u32-read-us,u64-hash',frame_sample_count=len(frames),
                          frame_samples_base64=base64.b64encode(b''.join(gpu.UI_FRAME.pack(*row) for row in frames)).decode(),
                          motion_sample_ranges=[[i*43,i*43+42] for i in range(count)],motion_steps=42,motion_warmup_steps=6)
            record.update(observation_schema=2,observer_independent=True,observer_policy_passes=count,
                          input_pacing='absolute-monotonic-no-catchup',input_sample_count=count*43,
                          input_samples_base64=base64.b64encode(b''.join(gpu.UI_INPUT.pack(row[0]) for row in frames)).decode())
        if spec['category']=='performance':
            vk=case_id<43
            record.update(**{key:gpu.CATALOG[key] for key in ('width','height','frames','warmup','target_fps')},
                          layers=(0,8,32)[(case_id-40)%3],
                          sample_columns=['encode_us','submit_us','wait_us','total_us','gpu_us'] if vk else ['encode_us','commit_and_complete_us','total_us'],
                          samples=[[1000,1000,6000,8000,7000] if vk else [1000,7000,8000] for _ in range(240)])
        elapsed=(240*8000*1000+24*8000*1000+1000000 if spec['category']=='performance' else
                 len(frames)*8333*1000+1000000 if spec['category']=='ui' else 1000000)
        record.update(end_ns=start+elapsed,duration_us=elapsed//1000)
        rows.append(dict(kind='begin',id=case_id,start_ns=start))
        if case_id==14:
            rows.append(dict(kind='device',api='vulkan',vendor=0x5143,chip_id=0x07030002,software_renderer=False,
                             name='Adreno730v3',tu_debug=('nocb','sysmem,nocb','gmem,nocb')[policy]))
        if case_id==30:
            rows.append(dict(kind='device',api='metal',name='Adreno730v3',class_='NativeBridgeMetalDevice',
                             implementation='/System/Library/Extensions/DitingAdrenoMetal.bundle/DitingMetalBridge.dylib',
                             tu_debug=('nocb','sysmem,nocb','gmem,nocb')[policy],single_sample_override='0'))
            rows[-1]['class']=rows[-1].pop('class_')
        rows.extend([dict(kind='end',id=case_id,start_ns=start,end_ns=record['end_ns']),record]);start=record['end_ns']+1000000
    rows.append(dict(kind='summary',schema=1,job=job,cases=len(gpu.expected_cases(profile,policy)),failures=0,
                     pass_=True,hardware_performance_pass=False,physical_present='UNQUALIFIED'))
    rows[-1]['pass']=rows[-1].pop('pass_');return rows

def ndjson(rows):return b''.join(json.dumps(row,separators=(',',':'),allow_nan=False).encode()+b'\n' for row in rows)

class PtyDevice:
    def __init__(self,respond):
        self.master,self.slave=pty.openpty();self.port=os.ttyname(self.slave);self.respond=respond
        self.commands=[];self.stop=threading.Event();self.error=None
        self.thread=threading.Thread(target=self.serve,daemon=True);self.thread.start()
    def serve(self):
        pending=bytearray()
        try:
            while not self.stop.is_set():
                if not select.select([self.master],[],[],.05)[0]:continue
                pending.extend(os.read(self.master,4096))
                while b'\n' in pending:
                    line,_,rest=pending.partition(b'\n');pending=bytearray(rest)
                    if not line or line==b'\x15':continue
                    command=line.decode('ascii');self.commands.append(command);response=self.respond(command)
                    if response is None:continue
                    response=b'kernel diagnostic\n'+response
                    for at in range(0,len(response),79):os.write(self.master,response[at:at+79])
        except Exception as error:self.error=error
    def close(self):
        self.stop.set();self.thread.join(1);os.close(self.master);os.close(self.slave)
        if self.error:raise self.error

class AutomationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory=tempfile.TemporaryDirectory(prefix='owned-gpu-automation-');cls.addClassCleanup(cls.directory.cleanup)
        cls.root=Path(cls.directory.name);flags=['-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined']
        cls.broker=cls.root/'broker';cls.process=cls.root/'process';cls.display=cls.root/'display'
        subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17',*flags,'-pthread',str(ROOT/'tests/diting_gpu_automation.cpp'),'-o',str(cls.broker)],check=True,capture_output=True)
        subprocess.run([os.environ.get('CC','clang'),'-std=c11','-D_GNU_SOURCE',*flags,str(ROOT/'tests/diting_gpu_process.c'),'-o',str(cls.process)],check=True,capture_output=True)
        subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17',*flags,'-I'+str(ROOT/'drivers/DitingPower'),
                        '-I'+str(ROOT/'drivers/DitingUsb'),str(ROOT/'tests/diting_display.cpp'),'-o',str(cls.display)],check=True,capture_output=True)
        cls.display_reply=subprocess.check_output([str(cls.display),'reply'])

    def test_real_state_command_parser_broker_and_cpu_oracle(self):
        for case in ('protocol','lifecycle','quiescence','overflow','replacement','broker','oracle','cadence'):
            with self.subTest(case=case):
                result=subprocess.run([str(self.broker),case],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(result.stdout.strip(),'PASS '+case)

    def test_real_process_success_crash_timeout_cancel_overflow_and_fd_cleanup(self):
        for case in ('success','exit','crash','hang','cancel','reject','overflow','eof','closed-stdout','lease','leaks','missing','interrupt'):
            with self.subTest(case=case):
                result=subprocess.run([str(self.process),case,str(self.root/'process.lock')],capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stderr);self.assertTrue(result.stdout.startswith('PASS '+case+' '),result.stdout)

    def test_orphan_worker_retains_lease_after_supervisor_exit(self):
        path=self.root/'orphan.lock'
        process=subprocess.Popen([str(self.process),'lease-orphan',str(path)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.addCleanup(process.stdout.close);self.addCleanup(process.stderr.close)
        line=process.stdout.readline();pid=int(line.split()[1]);self.assertEqual(process.wait(timeout=3),0)
        with path.open('rb') as lease:
            with self.assertRaises(BlockingIOError):fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB)
            process.stdout.read();fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB)
        self.assertGreater(pid,0)

    def test_default_matrix_contains_each_required_case(self):
        self.assertEqual(len(gpu.expected_cases(1,0)),40);self.assertEqual(len(gpu.expected_cases(2,0)),8)
        self.assertEqual(len(gpu.expected_cases(3,0)),50);self.assertEqual(len(gpu.expected_cases(3,1)),46)
        for profile,policy in ((1,0),(2,1),(3,0),(3,2),(4,0)):
            self.assertTrue(gpu.decode_result(ndjson(fixture(profile,policy)),1,profile,policy)['pass_'])

    def test_partial_render_requires_raw_storage_cycles_and_executed_draw_transfers(self):
        for case_id,key,value in ((34,'linear_storage_pixels',[]),(35,'partial_render_area',False),
                                  (36,'gmem_destination_delta',[0]*12),(37,'reuse_cycles',5),
                                  (34,'draw_transfer_verified',False),(37,'seed_pixels',{})):
            with self.subTest(case=case_id,key=key):
                rows=fixture(1,2)
                case=next(row for row in rows if row.get('id')==case_id and row['kind']=='case')
                case[key]=value
                with self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,1,2)

    def test_linear_fetch_and_reuse_require_executed_paths_and_every_pixel_check(self):
        for case_id,key,value in ((15,'render_mode_delta',[0]*8),(17,'gmem_side_delta',[0]*16),
                                  (18,'path_verified',False),(31,'reuse_cycles',3),
                                  (32,'cycle_pixels',[]),(33,'readback_source','texture-copy'),
                                  (15,'gmem_event_delta',[]),(19,'seed_pixels',{}),(31,'linear_tiling',True)):
            with self.subTest(case=case_id,key=key):
                rows=fixture(1,2);case=next(row for row in rows if row.get('id')==case_id and row['kind']=='case')
                case[key]=value
                with self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,1,2)
        rows=fixture(1,2);case=next(row for row in rows if row.get('id')==31 and row['kind']=='case')
        case['cycle_pixels'][0]['guard_errors']=1
        with self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,1,2)

    def test_host_import_requires_original_storage_metadata_and_six_draw_loads(self):
        for case_id,key,value in ((46,'imported_host_pointer',False),(47,'external_handle_type',0),
                                  (48,'host_pointer_alignment',3),(46,'host_allocation_bytes',1024),
                                  (47,'draw_load_required',False),(48,'readback_source','Vulkan transfer buffer'),
                                  (46,'gmem_destination_delta',[6,0,0,6,0,0,0,0,0,0,0,0]),
                                  (47,'gmem_side_delta',[0]*16)):
            with self.subTest(case=case_id,key=key):
                rows=fixture(1,2)
                case=next(row for row in rows if row.get('id')==case_id and row['kind']=='case')
                case[key]=value
                with self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,1,2)

    def test_ordinary_partial_render_has_event_load_and_draw_store_coverage(self):
        rows=fixture(1,2)
        for case in rows:
            if case['kind']=='case' and case.get('id') in (34,35,36,37):
                case['gmem_side_delta']=[0]*16
                case['gmem_event_delta']=[6,0,0,0 if case['id']==37 else 6,1,0,0,0 if case['id']==37 else 1]
        self.assertTrue(gpu.decode_result(ndjson(rows),1,1,2)['pass_'])

    def test_event_only_linear_transfer_and_shadow_iosurface_are_qualified_separately(self):
        rows=fixture(1,2)
        for case in rows:
            if case['kind']=='case' and case.get('id') in (15,16,19):
                case['gmem_side_delta']=[0]*16
                case['gmem_event_delta']=[1,1,0,1,2,2,0,2]
        self.assertTrue(gpu.decode_result(ndjson(rows),1,1,2)['pass_'])
        case=next(row for row in rows if row.get('id')==15 and row['kind']=='case')
        case['seed_pixels']['mismatches']=18604
        with self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,1,2)

    def test_host_p95_failure_preserves_original_device_summary(self):
        rows=fixture(2);case=next(row for row in rows if row.get('id')==40 and row['kind']=='case')
        for row in case['samples'][-13:]:row[2]=7000;row[3]=9000;row[4]=8500
        raw=ndjson(rows);result=gpu.decode_result(raw,1,2,0)
        checked=next(case for case in result['cases'] if case['id']==40)
        self.assertFalse(checked['pass']);self.assertTrue(checked['device_pass']);self.assertEqual(checked['evaluated']['p95_ms'],9)
        self.assertEqual(result['summary']['failures'],0);self.assertTrue(result['summary']['pass']);self.assertFalse(result['pass_'])
        self.assertEqual(raw,ndjson(rows))

    def test_percentile_uses_full_fixed_sample_count_and_120fps_budget(self):
        rows=fixture(2);case=next(row for row in rows if row.get('id')==40 and row['kind']=='case')
        for row in case['samples'][-12:]:row[2]=7000;row[3]=9000;row[4]=8500
        result=gpu.decode_result(ndjson(rows),1,2,0);self.assertTrue(result['pass_'])
        case['target_fps']=60
        with self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,2,0)

    def test_backend_policy_shader_and_sample_identity_are_mandatory(self):
        changes=[('vulkan','vendor',0),('vulkan','chip_id',0x07030001),('vulkan','software_renderer',True),
                 ('vulkan','tu_debug','sysmem,nocb'),('metal','tu_debug','gmem,nocb'),('metal','class','ForeignDevice'),
                 ('metal','implementation','/software/Metal'),('metal','single_sample_override','1')]
        for api,key,value in changes:
            rows=fixture();next(row for row in rows if row.get('api')==api)[key]=value
            with self.subTest(api=api,key=key),self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,1,0)
        rows=fixture();rows[0]['host_supplied_shader']=True
        with self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,1,0)

    def test_pixel_guards_sync_and_ui_cannot_pass_on_missing_or_unchanged_evidence(self):
        for case_id,key,value in ((1,'pixels',1),(5,'max_channel_error',1),(6,'guard_errors',1),(12,'mismatches',1),(13,'cycles',63)):
            rows=fixture();next(row for row in rows if row.get('id')==case_id and row['kind']=='case')[key]=value
            with self.subTest(case=case_id),self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,1,0)
        rows=fixture(4);case=next(row for row in rows if row.get('id')==60 and row['kind']=='case');case['content_hashes']=[1,1,1]
        with self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,4,0)

    def test_changed_endpoints_and_60hz_motion_cannot_qualify_continuous_120hz_animation(self):
        for stride in (2,42):
            rows=fixture(4);case=next(row for row in rows if row.get('id')==60 and row['kind']=='case')
            frames=list(gpu.UI_FRAME.iter_unpack(base64.b64decode(case['frame_samples_base64'])))
            case['frame_samples_base64']=base64.b64encode(b''.join(gpu.UI_FRAME.pack(at,read,i//stride)
                for i,(at,read,_) in enumerate(frames))).decode()
            result=gpu.decode_result(ndjson(rows),1,4,0)
            observed=next(row for row in result['cases'] if row['id']==60)['animation_observation']
            self.assertEqual(observed['status'],'FAIL');self.assertTrue(result['pass_'])
            report=gpu.assess(dict(jobs=[dict(policy='auto',profile='ui',state='PASSED',terminal=dict(state=3),assessment=result)]))
            self.assertEqual(report['assessment'],'FAIL');self.assertTrue(any('continuous' in text for text in report['failures']))
            self.assertFalse(report['hardware_performance_pass'])

    def test_late_observer_is_incomplete_instead_of_a_fps_measurement(self):
        rows=fixture(4);case=next(row for row in rows if row.get('id')==60 and row['kind']=='case')
        frames=list(gpu.UI_FRAME.iter_unpack(base64.b64decode(case['frame_samples_base64'])))
        case['frame_samples_base64']=base64.b64encode(b''.join(gpu.UI_FRAME.pack(at*2,read,hash_)
            for at,read,hash_ in frames)).decode()
        case['end_ns']=case['start_ns']+len(frames)*16666*1000+1000000
        next(row for row in rows if row.get('id')==60 and row['kind']=='end')['end_ns']=case['end_ns']
        result=gpu.decode_result(ndjson(rows),1,4,0)
        checked=next(row for row in result['cases'] if row['id']==60)
        self.assertEqual(checked['animation_observation']['status'],'INCOMPLETE')
        report=gpu.assess(dict(jobs=[dict(policy='auto',profile='ui',state='PASSED',terminal=dict(state=3),assessment=result)]))
        self.assertEqual(report['assessment'],'INCOMPLETE');self.assertFalse(report['failures'])

    def test_timely_observer_cannot_hide_burst_or_late_input(self):
        for late_step in (1,13):
            rows=fixture(4);case=next(row for row in rows if row.get('id')==60 and row['kind']=='case')
            inputs=[at for at, in gpu.UI_INPUT.iter_unpack(base64.b64decode(case['input_samples_base64']))]
            # Model the old catch-up burst: the input thread sleeps 100 ms,
            # then sends thirteen positions almost simultaneously.
            for i in range(late_step,late_step+13):inputs[i]=inputs[late_step-1]+100000+i-late_step
            case['input_samples_base64']=base64.b64encode(b''.join(gpu.UI_INPUT.pack(at) for at in inputs)).decode()
            result=gpu.decode_result(ndjson(rows),1,4,0)
            animation=next(row for row in result['cases'] if row['id']==60)['animation_observation']
            self.assertEqual(animation['status'],'INCOMPLETE')
            self.assertTrue(animation['motions'][0]['continuous_content_observed'])

    def test_independent_input_stream_and_observer_identity_are_required(self):
        for key,value in (('input_samples_base64','AAAA'),('observer_independent',False),
                          ('input_sample_count',43),('observer_policy_passes',0),('input_pacing','catch-up')):
            rows=fixture(4);next(row for row in rows if row.get('id')==60 and row['kind']=='case')[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,4,0)

    def test_legacy_coupled_observer_cannot_qualify_animation(self):
        rows=fixture(4)
        for row in rows:
            if row.get('category')=='ui':row.pop('observation_schema')
        result=gpu.decode_result(ndjson(rows),1,4,0)
        self.assertTrue(all(row['animation_observation']['status']=='INCOMPLETE'
                            for row in result['cases'] if row['category']=='ui'))

    def test_resettable_read_pointer_counter_is_never_te_frequency(self):
        snapshot=gpu.DISPLAY.decode(self.display_reply.decode())
        self.assertNotIn('te_frame_counter_raw',snapshot['interfaces'][0])
        self.assertIn('rd_ptr_frame_counter_raw',snapshot['interfaces'][0])
        after=copy.deepcopy(snapshot)
        for key in ('cpu_start_us','cpu_end_us'):after['header'][key]=str(int(snapshot['header'][key])+1000000)
        after['interfaces'][0]['rd_ptr_frame_counter_raw']=0
        result=gpu.display_rates([snapshot,after])
        self.assertEqual(result['status'],'UNAVAILABLE');self.assertIsNone(result['external_te_median_hz'])

    def test_timestamp_cadence_calibration_rejects_sparse_drift_and_incomplete_data(self):
        def events(hz=60,step=4000,drift=False):
            rows=[]
            for us in range(1000000,9000001,step):
                ticks=int((us*hz//1000000)*(19200000/hz))
                if drift and us>5000000:ticks+=int((us-5000000)*5)
                rows.append(SimpleNamespace(event=26,us=us,values=(0,0,0,0,ticks,0,0,1,0,1,0,0)))
            return rows
        for hz in (60,120):
            result=gpu.display_timestamp_cadence(events(hz))
            self.assertEqual(result['status'],'BELOW_TARGET' if hz==60 else 'TARGET_RANGE')
            self.assertAlmostEqual(result['panel_timestamp_median_hz'],hz,delta=.1)
            self.assertAlmostEqual(result['calibration']['observed_raw_clock_hz'],19200000,delta=50000)
            self.assertIsNone(result['physical_composed_frames'])
        for rows in (events(step=100000),events()[:20],events(drift=True)):
            result=gpu.display_timestamp_cadence(rows)
            self.assertEqual(result['status'],'UNAVAILABLE');self.assertIsNone(result['panel_timestamp_median_hz'])

    def test_ui_frame_bytes_motion_ranges_and_clock_bounds_are_required(self):
        for key,value in (('frame_samples_base64','AAAA'),('frame_sample_count',1537),
                          ('motion_sample_ranges',[[0,41]]*8),('frame_sample_start_ns',0),('accepted_events',1)):
            rows=fixture(4);next(row for row in rows if row.get('id')==60 and row['kind']=='case')[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,4,0)

    def test_case_markers_join_worker_pid_and_keep_cp_and_cpu_clocks_separate(self):
        output=self.root/'trace-joins';output.mkdir()
        case=dict(id=1,name='vk-clear-rgba8',category='colour',start_ns=2000000000,end_ns=2200000000,**{'pass':True})
        events=[(2000100,64,99,[5,1,1,case['start_ns'],0]),
                (2001000,10,31,[1,0,0,0,0,0,0,0,2000500]),
                (2002000,10,42,[2,0,0,0,0,0,0,0,2001000]),
                (2011500,11,31,[1,0,0,2000500,19200,211200,1]),
                (2031000,11,42,[2,0,0,2001000,19200,403200,1]),
                (2200500,64,99,[5,1,2,case['start_ns'],case['end_ns']])]
        def write(events):
            records=b''.join(gpu.TRACE_RECORD.pack(i+1,us,event,pid,*(values+[0]*(12-len(values))))
                for i,(us,event,pid,values) in enumerate(events))
            (output/'trace.bin').write_bytes(gpu.TRACE_HEADER.pack(0x31545047,1,gpu.TRACE_RECORD.size,len(events),len(events),0)+records)
        write(events)
        report=dict(jobs=[dict(job=5,policy='auto',terminal=dict(daemon=99),assessment=dict(manifest=dict(pid=31),cases=[case]))])
        gpu.correlate_trace(output,report);trace=case['kernel_trace']
        self.assertEqual(trace['marker_status'],'PASS');self.assertEqual(trace['submissions'],1);self.assertEqual(trace['retirements'],1)
        self.assertEqual(trace['by_pid']['31']['cp_execution']['p95_ms'],10)
        self.assertEqual(trace['by_pid']['31']['submission_to_cpu_retire']['p95_ms'],11)
        self.assertFalse(report['scenario_trace_incomplete']);self.assertTrue((output/'decoded-trace.json').exists())
        damaged=copy.deepcopy(events);damaged[-1][3][4]+=1;write(damaged)
        gpu.correlate_trace(output,report);self.assertEqual(case['kernel_trace']['marker_status'],'INCOMPLETE')
        self.assertTrue(report['scenario_trace_incomplete'])

    def test_partial_crash_records_are_kept_but_never_pass(self):
        rows=fixture();partial=rows[:5]+[dict(kind='begin',id=1,start_ns=2000000000)]
        raw=ndjson(partial)+b'{"kind":"case","detail":"\xe4\xbd'
        result=gpu.decode_result(raw,1,1,0,allow_partial=True)
        self.assertFalse(result['pass_']);self.assertEqual(len(result['cases']),1);self.assertEqual(result['unfinished_cases'],[1])
        self.assertIsNone(result['summary']);self.assertGreater(result['truncated_tail_bytes'],0)
        with self.assertRaises(ValueError):gpu.decode_result(raw,1,1,0)
        with self.assertRaises(ValueError):gpu.decode_result(ndjson(partial)+b'{broken}\n',1,1,0,allow_partial=True)

    def test_duplicate_json_bad_summary_and_forged_physical_claim_are_rejected(self):
        raw=ndjson(fixture());raw=raw.replace(b'"schema":1',b'"schema":1,"schema":1',1)
        with self.assertRaises(ValueError):gpu.decode_result(raw,1,1,0)
        for field,value in (('failures',1),('hardware_performance_pass',True),('physical_present','PASS')):
            rows=fixture();rows[-1][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):gpu.decode_result(ndjson(rows),1,1,0)

    def test_chunk_framing_job_offset_base64_and_size_are_checked(self):
        command='gputest result 1 0 3';raw=frame(command,'GPUTEST schema=1 error=0 job=1 offset=0 bytes=3 total=3\nDATA AP8Q\n')
        self.assertEqual(gpu.result_chunk(raw,command,1,0,3,3),b'\x00\xff\x10')
        for bad in (raw.replace(b'job=1',b'job=2'),raw.replace(b'offset=0',b'offset=1'),raw.replace(b'bytes=3',b'bytes=2'),
                    raw.replace(b'AP8Q',b'!!!!'),raw[:-1],raw+raw):
            with self.assertRaises(ValueError):gpu.result_chunk(bad,command,1,0,3,3)

    def test_cli_rejects_shell_paths_unsupported_policy_and_large_chunks(self):
        self.assertEqual(cli.gpu_command(['run','all','gmem']),'gputest run all gmem')
        self.assertEqual(cli.gpu_command(['result','1','0000','1024']),'gputest result 1 0 1024')
        for args in (['run','ui','sysmem'],['run','shell','auto'],['cancel','-1'],['result','1','0','1025'],['status;reboot']):
            with self.assertRaises(ValueError):cli.gpu_command(args)

    def test_ci_reports_count_infrastructure_errors_and_incomplete_evidence(self):
        output=self.root/'reports';output.mkdir()
        report=gpu.assess(dict(jobs=[],execution='ERROR',infrastructure_error='USB disconnected',missing_positions=1))
        gpu.write_reports(output,report);suite=ET.parse(output/'junit.xml').getroot()
        self.assertEqual(suite.get('errors'),'1');self.assertEqual(int(suite.get('tests')),len(suite.findall('testcase')))
        self.assertGreater(int(suite.get('failures')),0);self.assertIn('USB disconnected',(output/'report.md').read_text())
        self.assertEqual(json.loads((output/'report.json').read_text())['assessment'],'FAIL')

    def test_pty_matrix_continues_after_quiesced_worker_crash(self):
        output=self.root/'matrix';output.mkdir();current=dict(job=0,state=0,profile=0,policy=0,raw=b'')
        def reply(command):
            if command.startswith('gputest run '):
                _,_,profile,policy=command.split();current.update(job=current['job']+1,state=1,profile=gpu.PROFILES[profile],policy=gpu.POLICIES[policy])
                rows=fixture(current['profile'],current['policy'],current['job'])
                current['raw']=ndjson(rows[:5]+[dict(kind='begin',id=1,start_ns=2000000000)]) if current['job']==1 else ndjson(rows)
            elif command=='gputest status' and current['job']:current['state']=4 if current['job']==1 else 3
            if command.startswith('gputest result '):
                _,_,job,offset,count=command.split();offset,count=int(offset),int(count);block=current['raw'][offset:offset+count]
                return frame(command,f'GPUTEST schema=1 error=0 job={job} offset={offset} bytes={len(block)} total={len(current["raw"])}\nDATA '+base64.b64encode(block).decode()+'\n')
            if command.startswith('gputest '):
                return frame(command,'GPUTEST schema=1 error=0 available=1 daemon=31 heartbeat_age_us=0 '
                    f'job={current["job"]} state={current["state"]} profile={current["profile"]} policy={current["policy"]} '
                    f'bytes={len(current["raw"]) if current["state"]>=3 else 0} case=0 stage=0 worker_error=0 '
                    f'created_us={current["job"]*1000000} completed_us=0 timeout_ms=120000 quiescing=0\n')
            if command.startswith('trace '):
                raw=gpu.STREAM.CHUNK.pack(0x31545047,1,gpu.TRACE_RECORD.size,0,0,0,1,0,0,0)
                return frame(command,f'GPU trace bytes={len(raw)}\nDATA '+base64.b64encode(raw).decode()+'\n')
            if command=='display':return self.display_reply
            return frame(command,'fixture prerequisite\n')
        device=PtyDevice(reply)
        try:
            with patch('diting_usb_session.fcntl.ioctl',return_value=0):
                with DitingUsbSession(device.port) as session:code=runner.run(session,output,'qualification',['auto','sysmem'],.5)
            self.assertEqual(code,2);report=json.loads((output/'report.json').read_text())
            self.assertEqual(report['execution'],'COMPLETE');self.assertEqual(len(report['jobs']),2)
            self.assertEqual(report['jobs'][0]['state'],'FAILED');self.assertTrue(report['jobs'][1]['assessment']['pass_'])
            self.assertFalse(any(command.startswith('gputest cancel') for command in device.commands))
            self.assertTrue((output/'auto-qualification-1.ndjson').is_file())
        finally:device.close()

    def test_partial_usb_timeout_cancels_only_owned_work_and_keeps_the_failure_report(self):
        output=self.root/'transport-timeout';output.mkdir();current=dict(job=0,state=0)
        def reply(command):
            if command.startswith('gputest run '):current.update(job=1,state=1)
            if command=='gputest cancel 1':current['state']=5
            if command.startswith('gputest '):
                return frame(command,'GPUTEST schema=1 error=0 available=1 daemon=31 heartbeat_age_us=0 '
                    f'job={current["job"]} state={current["state"]} profile={1 if current["job"] else 0} policy=0 '
                    'bytes=0 case=0 stage=0 worker_error=0 '
                    f'created_us={current["job"]*1000000} completed_us=0 timeout_ms=120000 quiescing=0\n')
            if command.startswith('trace '):
                if current['state']==1:
                    return ('DitingUSB BEGIN '+command+'\npartial GPU reply\n').encode()
                raw=gpu.STREAM.CHUNK.pack(0x31545047,1,gpu.TRACE_RECORD.size,0,0,0,1,0,0,0)
                return frame(command,f'GPU trace bytes={len(raw)}\nDATA '+base64.b64encode(raw).decode()+'\n')
            if command=='display':return self.display_reply
            return frame(command,'fixture prerequisite\n')
        device=PtyDevice(reply)
        try:
            with patch('diting_usb_session.fcntl.ioctl',return_value=0):
                with DitingUsbSession(device.port) as session:code=runner.run(session,output,'qualification',['auto','sysmem'],.2)
            report=json.loads((output/'report.json').read_text())
            self.assertEqual(code,3);self.assertEqual(report['execution'],'ERROR');self.assertEqual(len(report['jobs']),1)
            self.assertEqual(report['jobs'][0]['cancel_terminal']['state'],5)
            self.assertEqual(report['jobs'][0]['cancel_terminal']['quiescing'],0)
            self.assertEqual(device.commands.count('gputest cancel 1'),1)
            self.assertIn(b'partial GPU reply',(output/'usb.raw.log').read_bytes())
            self.assertTrue((output/'junit.xml').exists())
        finally:device.close()

    def test_real_owned_shaders_and_metallib_frontend_bounds(self):
        out=self.root/'shaders';commands=[]
        def run(command):
            command=list(map(str,command));subprocess.run(command,check=True,capture_output=True,timeout=20);commands.append(command)
        shaders(out,dict(os.environ),run)
        self.assertEqual(len(commands),11);self.assertTrue((out/'GpuTestShaders.h').is_file())
        checker=os.environ.get('DITING_GPU_AIR_CHECKER')
        if checker:
            result=subprocess.run([checker,str(out/'pattern.metallib')],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(result.stdout.count('PASS'),3)
            raw=(out/'pattern.metallib').read_bytes()
            for index,damaged in enumerate((raw[:-1],raw[:8]+b'\xff'*8+raw[16:],raw[:-1]+bytes([raw[-1]^1]))):
                path=out/f'damaged-{index}.metallib';path.write_bytes(damaged)
                self.assertNotEqual(subprocess.run([checker,str(path)],capture_output=True,timeout=20).returncode,0)
        for bad in (b'BC\xc0\xde',{},dict(vertex=b'wrong',fragment=b'wrong',compute=b'wrong')):
            with self.assertRaises(ValueError):metallib(bad)

if __name__=='__main__':unittest.main()
