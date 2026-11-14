"""Adapt the pinned Linux ath11k QMI BDF response to QCA6490 PA selection.

Wire evidence: Nothing CNSS2 c402fcdb cnss_utils/wlan_firmware_service_v01
uses optional TLV 0x10 (u64 host_bdf_data), total response length 18 bytes.
Only generated sources are edited; the reference checkout stays unchanged.
"""


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError('Pinned ath11k BDF ABI differs: ' + old[:80])
    return source.replace(old, new)


def adapt_bdf(source, header):
    header = replace_once(header, 'QMI_WLANFW_BDF_DOWNLOAD_RESP_MSG_V01_MAX_LEN\t7',
                          'QMI_WLANFW_BDF_DOWNLOAD_RESP_MSG_V01_MAX_LEN\t18')
    old = ('struct qmi_wlanfw_bdf_download_resp_msg_v01 {\n'
           '\tstruct qmi_response_type_v01 resp;\n};')
    header = replace_once(header, old, old[:-2] + '\tu8 host_bdf_data_valid;\n\tu64 host_bdf_data;\n};')
    begin = source.index('static const struct qmi_elem_info qmi_wlanfw_bdf_download_resp_msg_v01_ei[] = {')
    end = source.index('\n};', begin) + 3
    table = source[begin:end]
    marker = '\t{\n\t\t.data_type\t= QMI_EOTI,'
    added = '''\t{
        .data_type = QMI_OPT_FLAG,
        .elem_len = 1, .elem_size = sizeof(u8), .array_type = NO_ARRAY,
        .tlv_type = 0x10,
        .offset = offsetof(struct qmi_wlanfw_bdf_download_resp_msg_v01, host_bdf_data_valid),
    },
    {
        .data_type = QMI_UNSIGNED_8_BYTE,
        .elem_len = 1, .elem_size = sizeof(u64), .array_type = NO_ARRAY,
        .tlv_type = 0x10,
        .offset = offsetof(struct qmi_wlanfw_bdf_download_resp_msg_v01, host_bdf_data),
    },
'''
    source = source[:begin] + replace_once(table, marker, added + marker) + source[end:]
    source = '#include "DitingWifiBdf.h"\n' + source
    begin = source.index('static int ath11k_qmi_load_file_target_mem(')
    end = source.index('\nstatic int ath11k_qmi_load_bdf_qmi(', begin)
    function = source[begin:end]
    function = replace_once(function, '\twhile (remaining) {',
                            '\twhile (remaining) {\n        memset(&resp, 0, sizeof(resp));')
    marker = '\n\t\tif (ab->hw_params.fixed_bdf_addr ||\n\t\t    type == ATH11K_QMI_FILE_TYPE_EEPROM) {\n\t\t\tremaining = 0;'
    added = '''
        if (req->end && (type == ATH11K_QMI_BDF_TYPE_ELF || type == ATH11K_QMI_BDF_TYPE_BIN)) {
            ret = diting_wifi_bdf_power_config(resp.host_bdf_data_valid, resp.host_bdf_data);
            if (ret) {
                ath11k_warn(ab, "QCA6490 BDF PA configuration failed: %d\\n", ret);
                goto err_iounmap;
            }
        }
'''
    function = replace_once(function, marker, added + marker)
    return source[:begin] + function + source[end:], header
