"""Locate Diting's retained CDC interface even when USB uses the iOS UDID."""
import glob
from pathlib import Path
import plistlib
import subprocess
import sys

def _linux_usb_nodes():
    devices={}
    for tty in Path('/sys/class/tty').glob('ttyACM*'):
        interface=(tty/'device').resolve()
        try:
            if ((interface/'bInterfaceClass').read_text().strip(),
                (interface/'bInterfaceSubClass').read_text().strip())!=('02','02'):
                continue
            device=next(p for p in interface.parents if (p/'idVendor').is_file())
            serial=(device/'serial').read_text().strip()
            identity=((device/'idVendor').read_text().strip(),
                      (device/'idProduct').read_text().strip())
            if identity!=('05ac','12a8') and not (identity==('ffff','0128') and serial=='DITING-SM8475'):
                continue
        except (OSError,StopIteration):
            continue
        node=devices.setdefault(str(device),{'USB Serial Number':serial,'IORegistryEntryChildren':[]})
        node['IORegistryEntryChildren'].append({'IOCalloutDevice':'/dev/'+tty.name})
    return list(devices.values())

def _nodes(value):
    if isinstance(value,dict):
        yield value
        for child in value.get('IORegistryEntryChildren',[]):yield from _nodes(child)
    elif isinstance(value,list):
        for child in value:yield from _nodes(child)

def phone_usb_nodes():
    if sys.platform.startswith('linux'):
        return _linux_usb_nodes()
    try:
        root=plistlib.loads(subprocess.check_output(
            ['/usr/sbin/ioreg','-a','-r','-c','IOUSBHostDevice','-l','-w','0'],timeout=5))
    except (OSError,ValueError,subprocess.SubprocessError,plistlib.InvalidFileException):return []
    found=[];seen=set()
    for node in _nodes(root):
        # Interface objects inherit VID/PID too; select the physical device
        # once rather than counting its CDC child as another phone.
        if 'bDeviceClass' not in node or node.get('idVendor')!=0x05ac or node.get('idProduct')!=0x12a8:continue
        key=(node.get('IORegistryEntryID'),node.get('locationID'),node.get('USB Serial Number'))
        if key in seen:continue
        if any(any(isinstance(v,str) and 'Diting XNU console' in v for v in item.values()) or
               (item.get('bInterfaceClass')==2 and item.get('bInterfaceSubClass')==2)
               for item in _nodes(node)):
            found.append(node);seen.add(key)
    return found

def serial_ports():
    ports=glob.glob('/dev/cu.usbmodemDITING_SM8475*')
    for node in phone_usb_nodes():
        for item in _nodes(node):
            port=item.get('IOCalloutDevice')
            if isinstance(port,str) and port.startswith(('/dev/cu.usbmodem','/dev/ttyACM')):ports.append(port)
    return sorted(set(ports))

def native_serial():
    nodes=phone_usb_nodes()
    if len(nodes)==1:return nodes[0].get('USB Serial Number') or nodes[0].get('kUSBSerialNumberString')
    return None
