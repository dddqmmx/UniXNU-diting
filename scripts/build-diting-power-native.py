#!/usr/bin/env python3
"""Build SM8475 ADSP/GLINK, Xiaomi battery protocol and audio GPR for Darwin24.
Firmware is included only through explicit local DITING_ADSP_FIRMWARE.
No firmware is written to Git. No phone I/O or image audits.
"""
import argparse,os,json,struct,subprocess
import sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import diting_paths
from diting_rpmh_contract import validate_live_dtb
from diting_adsp_firmware import prepare as prepare_adsp_firmware
def late_init(obj, filename, symbol):
 raw=bytearray(obj.read_bytes());at=32;sym=None
 for _ in range(struct.unpack_from('<I',raw,16)[0]):
  kind,length=struct.unpack_from('<II',raw,at)
  if kind==0x19:
   for i in range(struct.unpack_from('<I',raw,at+64)[0]):
    sc=at+72+80*i
    if raw[sc:sc+16].rstrip(b'\0')==b'__mod_init_func':
     raw[sc:sc+16]=b'__diting_init'.ljust(16,b'\0');v=struct.unpack_from('<I',raw,sc+64)[0];struct.pack_into('<I',raw,sc+64,v&~255)
  elif kind==2:sym=at
  at+=length
 if sym is None:raise RuntimeError('Battery object has no symbol table')
 so,count,st,size=struct.unpack_from('<4I',raw,sym+8)
 if st+size!=len(raw):raise RuntimeError('Unexpected owned battery object layout')
 for i in range(count):
  at=so+16*i;p=st+struct.unpack_from('<I',raw,at)[0];z=raw.index(0,p)
  if raw[p:z]==('__GLOBAL__sub_I_'+filename).encode():
   new=('_'+symbol+'\0').encode();struct.pack_into('<I',raw,at,size);raw[at+4]|=1;raw.extend(new);struct.pack_into('<I',raw,sym+20,size+len(new));break
 else:raise RuntimeError('Battery initializer missing')
 dest=obj.with_suffix('.late.o');dest.write_bytes(raw);return dest

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--tree',type=Path,required=True)
 mode=p.add_mutually_exclusive_group()
 mode.add_argument('--resources-only',action='store_true',help='compile only DitingPlatformResources.cpp: the shared SMMU/power locks the UFS transport takes, without the audio firmware input')
 mode.add_argument('--battery-only',action='store_true',help='compile the PMIC codec and IOPMPowerSource into a separate libDitingBattery.a for isolated compile checks')
 a=p.parse_args()
 out=a.tree/'build/diting-power';out.mkdir(parents=True,exist_ok=True)
 validate_live_dtb(out/'rpmh-contract.json')
 adsp_report,adsp_assembly=prepare_adsp_firmware(out,os.environ) if not a.resources_only and not a.battery_only else ({'builtin':False},None)
 framework=a.tree/'.cache/fakeroot/System/Library/Frameworks/Kernel.framework/Versions/A'
 env=dict(os.environ,DEVELOPER_DIR=str(diting_paths.developer_dir()))
 sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],env=env,text=True).strip()
 flags=['-target','arm64-apple-ios18.0','-isysroot',sdk,'-mkernel','-ffreestanding','-fno-builtin','-fno-stack-protector','-mgeneral-regs-only','-fblocks','-O2','-DKERNEL=1','-DKERNEL_PRIVATE=1','-DXNU_KERNEL_PRIVATE=1','-DARM64_BOARD_CONFIG_DITING=1','-I'+str(framework/'PrivateHeaders'),'-I'+str(framework/'Headers'),'-I'+str(ROOT/'drivers/DitingPower'),'-I'+str(out),'-I'+str(diting_paths.sheng_root()/'package'),'-Wno-deprecated-declarations']
 if env.get('XNU_DITING_RAMOOPS_CONSOLE')=='1':
  base=int(env['XNU_DITING_RAMOOPS_BASE'],0)
  if not 0<base<2**64 or base%4096:raise RuntimeError('Invalid configured Diting ramoops base')
  flags+=['-DXNU_DITING_RAMOOPS_CONSOLE=1','-DXNU_DITING_RAMOOPS_BASE='+hex(base)]
 if not a.resources_only and not a.battery_only:
  subprocess.run([os.sys.executable,str(ROOT/'scripts/prepare-diting-audio.py'),'--container',str(diting_paths.audio_inputs()/'tfa98xx.cnt'),'--output',str(out/'DitingAmpProfiles.h')],check=True)
 objects=[]
 flags+=['-DDITING_ADSP_BUILTIN='+str(int(adsp_report['builtin']))]
 if adsp_assembly:
  obj=out/'DitingAdspFirmware.o'
  subprocess.run(['xcrun','clang','-target','arm64-apple-ios18.0','-c',str(adsp_assembly),'-o',str(obj)],env=env,check=True)
  objects.append(obj)
 shared=diting_paths.sheng_root()/'package'
 NAMES=('DitingPlatformResources.cpp',) if a.resources_only else ('ShengBattmgr.c','DitingBattery.cpp') if a.battery_only else ('QcomRpmh.c','QcomSmem.c','QcomSmp2p.c','QcomGlink.c','QcomMdt.c','QcomQmp.c','ShengBattmgr.c','ShengGpr.c','ShengAudioReach.c','GeniI2c.c','DitingFirmware.c','DitingAdsp.cpp','DitingBattery.cpp','DitingAudio.cpp','DitingAudioService.cpp','DitingPlatformResources.cpp','DitingPower.cpp','DitingBacklight.cpp')
 for name in NAMES:
  source=(shared if name=='QcomRpmh.c' else ROOT/'drivers/DitingPower')/name;obj=out/(source.stem+'.o');cpp=source.suffix=='.cpp'
  cmd=['xcrun','clang++' if cpp else 'clang',*flags,*(['-std=gnu++17','-fno-exceptions','-fno-rtti'] if cpp else []),'-c',str(source),'-o',str(obj)]
  r=subprocess.run(cmd,env=env,capture_output=True,text=True);(out/(source.stem+'.log')).write_text(r.stdout+r.stderr)
  if r.returncode:raise SystemExit(r.stderr)
  if name=='DitingBattery.cpp':obj=late_init(obj,name,'DitingBatteryStaticInit')
  if name=='DitingAudioService.cpp':obj=late_init(obj,name,'DitingAudioServiceStaticInit')
  if name=='DitingBacklight.cpp':obj=late_init(obj,name,'DitingBacklightStaticInit')
  objects.append(obj)
 archive=out/('libDitingBattery.a' if a.battery_only else 'libDitingPower.a');archive.unlink(missing_ok=True)
 subprocess.run(['xcrun','libtool','-static','-o',str(archive),*map(str,objects)],env=env,check=True)
 (out/'build-report.json').write_text(json.dumps(dict(board='SM8475 Diting',guest='22H374',scope='battery-only' if a.battery_only else 'resources-only' if a.resources_only else 'power',adsp_pas=1,adsp_carveout=[adsp_report['board']['base'],adsp_report['board']['size']] if 'board' in adsp_report else None,ipcc=0xed18000,smem=0x80900000,battery='Xiaomi SM8450 PMIC GLINK, single pack, read only',battery_protocol_revision='eff4b40407a1b6f1b1ab84554cbb540bcb495175',audio='adsp_apps APM graphs, SID1801, TFA9873 stereo PCM ring; no microphone endpoint',firmware=adsp_report,runtime_observed=False),indent=2)+'\n')
 print(archive)
if __name__=='__main__':main()
