"""Build QCA6490 power inputs from its selected live DT and CNSS contract.

The primary regulator list and conditional IPA path follow Nothing CNSS2
c402fcdb3a1523d10b15ef1123bced45456f7592 (power.c and qmi.c).
Supply enumeration alone does not authorize an active enable request.
"""
import copy
import json
import re
import struct

# Preserve the vendor consumer order. IPA is deliberately a separate owner,
# selected by the firmware's final host_bdf_data response, not need_unvote.
PRIMARY = ('vdd-wlan-core', 'vdd-wlan-io', 'vdd-wlan-xtal-aon',
           'vdd-wlan-xtal', 'vdd-wlan', 'vdd-wlan-ctrl1', 'vdd-wlan-ctrl2',
           'vdd-wlan-sp2t', 'wlan-ant-switch', 'wlan-soc-swreg',
           'vdd-wlan-aon', 'vdd-wlan-dig', 'vdd-wlan-rfa1', 'vdd-wlan-rfa2',
           'vdd-wlan-en')
REQUIRED = {'vdd-wlan-io', 'wlan-ant-switch', 'vdd-wlan-aon',
            'vdd-wlan-dig', 'vdd-wlan-rfa1', 'vdd-wlan-rfa2'}
MODE_MAP = {'pmic4-ldo': {1: 4, 2: 5, 4: 7},
            'pmic5-ldo': {1: 3, 2: 4, 4: 7},
            'pmic4-smps': {1: 4, 2: 5, 3: 6, 4: 7},
            'pmic5-hfsmps': {1: 3, 2: 4, 3: 6, 4: 7}}


def cells(p, key):
    raw = p[key]
    if len(raw) % 4:
        raise ValueError('Malformed power DT cells: ' + key)
    return list(struct.unpack('>' + 'I' * (len(raw) // 4), raw))


def text(p, key):
    return p[key].rstrip(b'\0').decode('ascii')


def select_power_inputs(profile, nodes):
    refs = {cells(p, 'phandle')[0]: k for k, p in nodes.items() if 'phandle' in p}
    wlan_path = profile['source']['wlan']
    host_path = profile['source']['pcie']
    wlan, host = nodes[wlan_path], nodes[host_path]
    if (wlan.get('compatible') != b'qcom,cnss-qca6490\0' or
            cells(wlan, 'qcom,wlan-rc-num') != [0] or cells(host, 'cell-index') != [0]):
        raise ValueError('Selected DT needs a different WLAN/PCIe owner')
    old = {(e['supply'], e['resource'], e.get('regulator')): e
           for e in profile['rails']}

    def rail(supply, request, consumer):
        handle = cells(consumer, supply + '-supply')
        if len(handle) != 1 or handle[0] not in refs:
            raise ValueError('Invalid supply provider: ' + supply)
        path = refs[handle[0]]
        p, parent = nodes[path], nodes[path.rsplit('/', 1)[0]]
        if parent.get('compatible') != b'qcom,rpmh-vrm-regulator\0':
            raise ValueError('Supply is not an RPMh VRM: ' + supply)
        minimum, maximum, load, delay, need_unvote = request
        if (not 0 < minimum <= maximum or load or delay > 100000 or need_unvote not in (0, 1) or
                minimum < cells(p, 'regulator-min-microvolt')[0] or
                maximum > cells(p, 'regulator-max-microvolt')[0]):
            raise ValueError('Unsupported or out-of-range regulator request: ' + supply)
        # CNSS requests no regulator_set_mode when load_ua is zero. Only an
        # explicit provider init-mode may produce a raw mode vote here.
        mode = -1
        if 'qcom,init-mode' in p:
            generic = cells(p, 'qcom,init-mode')[0]
            mapping = MODE_MAP.get(text(parent, 'qcom,regulator-type'))
            if mapping is None or generic not in mapping:
                raise ValueError('Unknown requested PMIC mode: ' + supply)
            if generic not in cells(parent, 'qcom,supported-modes'):
                raise ValueError('Provider does not support requested mode: ' + supply)
            mode = mapping[generic]
        if 'vin-supply' in p or 'vin-supply' in parent:
            raise ValueError('Supply needs a parent-regulator implementation: ' + supply)
        entry = dict(supply=supply, resource=text(parent, 'qcom,resource-name'),
                     regulator=text(p, 'regulator-name'), request=request,
                     mode=mode, provider=path)
        prior = old.get((supply, entry['resource'], entry['regulator']), {})
        if 'physical' in prior:
            entry['physical'] = copy.deepcopy(prior['physical'])
        return entry

    supplied = {k[:-7] for k in wlan if k.endswith('-supply')}
    if supplied - set(PRIMARY) - {'vdd-wlan-ipa'} or not REQUIRED <= supplied:
        raise ValueError('WLAN primary consumer set differs from the SM8475 CNSS contract')
    boot = [rail(s, cells(wlan, 'qcom,' + s + '-config'), wlan)
            for s in PRIMARY if s in supplied]
    for s in ('vreg-1p8', 'vreg-0p9'):
        voltage = cells(host, 'qcom,' + s + '-voltage-level')[0]
        boot.append(rail(s, [voltage, voltage, 0, 0, 0], host))

    ipa = rail('vdd-wlan-ipa', cells(wlan, 'qcom,vdd-wlan-ipa-config'), wlan)
    name = text(wlan, 'qcom,vreg_ipa')
    if name != 's3e' or ipa['resource'] != 'smpe3':
        raise ValueError('IPA resource needs a different AOP configuration')
    # CNSS defaults to the RF PDC unless a vreg-pdc map explicitly selects it.
    pdc = 'rf'
    if 'qcom,vreg_pdc_map' in wlan:
        entries = text(wlan, 'qcom,vreg_pdc_map').split('\0')
        if len(entries) % 2:
            raise ValueError('Malformed WLAN PDC map')
        for key, value in zip(entries[::2], entries[1::2]):
            if name in key:
                pdc = value
                break
    if not re.fullmatch(r'[a-z0-9_]+', pdc):
        raise ValueError('Invalid WLAN PDC name')
    mailbox = refs[cells(wlan, 'mboxes')[0]]
    aoss = nodes[refs[cells(nodes[mailbox], 'qcom,qmp')[0]]]
    if (nodes[mailbox].get('compatible') != b'qcom,qmp-mbox\0' or
            cells(aoss, 'reg') != [0xc300000, 0x400]):
        raise ValueError('Selected WLAN mailbox differs from shared AOSS')
    ipa.update(owner='firmware BDF host_bdf_data + AOP wlan_pdc', active_vote=False,
               message=f'{{class: wlan_pdc, ss: {pdc}, res: {name}.e, upval: 1}}', wire_bytes=64)
    if len(ipa['message'].encode('ascii')) >= ipa['wire_bytes']:
        raise ValueError('IPA message exceeds CNSS mailbox wire length')
    return dict(boot_rails=boot, conditional_ipa=ipa,
                source='Nothing CNSS2 c402fcdb power.c/qmi.c and selected live DT',
                mode_policy='explicit provider init-mode only; preserve unrequested mode')


def power_header(contract):
    result = ['struct DitingWifiRail {const char *name;unsigned mv,maxMv,ppid,type,subtype;int mode;unsigned delayUs;};',
              'static constexpr DitingWifiRail kDitingWifiRails[]={']
    for entry in contract['boot_rails']:
        physical = entry.get('physical', {})
        ppid = (physical.get('spmi_slave', 0) << 8) | physical.get('peripheral', 0)
        request = entry['request']
        result.append('{%s,%d,%d,%d,%d,%d,%d,%d},' %
                      (json.dumps(entry['resource']), (request[0] + 999) // 1000, request[1] // 1000,
                       ppid, physical.get('type', 0), physical.get('subtype', 0), entry['mode'], request[3]))
    ipa = contract['conditional_ipa']
    result += ['};', 'static constexpr char kDitingWifiIpaMessage[]=' + json.dumps(ipa['message']) + ';',
               'static constexpr unsigned kDitingWifiIpaWireBytes=' + str(ipa['wire_bytes']) + ';']
    return '\n'.join(result) + '\n'
