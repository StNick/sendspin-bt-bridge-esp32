"""Adapt the receiver project's checked SDK overlay for LDAC transmission.

Generate private copies; never modify the shared ESP-IDF package. Every SDK
anchor must match exactly or configuration fails before writing any output.
"""
import argparse
from pathlib import Path
import re

SOURCES = ("btc_av.c", "bta_av_co.c", "bta_av_aact.c", "btc_a2dp_source_ext_codec.c", "l2c_link.c",
           "avdt_msg.c")
SOURCE_DIR = "components/bt/host/bluedroid/btc/profile/std/a2dp"


def source_path(framework, name):
    directory = {"bta_av_aact.c": "components/bt/host/bluedroid/bta/av",
                 "l2c_link.c": "components/bt/host/bluedroid/stack/l2cap",
                 "avdt_msg.c": "components/bt/host/bluedroid/stack/avdt"}.get(name, SOURCE_DIR)
    return Path(framework) / directory / name


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one SDK anchor, found {text.count(old)}: {old[:100]!r}")
    return text.replace(old, new, 1)


def function_span(text, name):
    clean = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                   lambda m: re.sub(r'[^\n]', ' ', m.group()), text, flags=re.S)
    match = re.search(r'^(?:static\s+)?[A-Za-z_]\w*\s+(?:\*\s*)?' + re.escape(name) +
                      r'\s*\([^;{}]*\)\s*\{', clean, flags=re.M)
    if not match:
        raise ValueError(f"SDK function not found: {name}")
    start = clean.index('{', match.start())
    depth, end = 1, start + 1
    while depth and end < len(clean):
        depth += (clean[end] == '{') - (clean[end] == '}')
        end += 1
    if depth:
        raise ValueError(f"Unbalanced SDK function: {name}")
    return start, end


def edit_function(text, name, transform):
    start, end = function_span(text, name)
    return text[:start] + transform(text[start:end]) + text[end:]


def add_case(text, name, case):
    return edit_function(text, name, lambda body: replace_once(body, '    default:', case + '\n    default:'))


def patch_btc_av(text):
    text = re.sub(r'\bBTC_AV_CODEC_NONE\b', 'REPO_BT_CODEC_NONE', text)
    text = add_case(text, 'btc_av_reg_sep', '''        case ESP_A2D_MCT_NON_A2DP: {
            if (tsep != AVDT_TSEP_SRC || !repo_ldac_cie_valid((const uint8_t *)&mcc->cie, false)) {
                param.a2d_sep_reg_stat.reg_state = ESP_A2D_SEP_REG_UNSUPPORTED;
                btc_a2d_cb_to_app(ESP_A2D_SEP_REG_STATE_EVT, &param);
                return;
            }
            codec_type = REPO_LDAC_CODEC_ID;
            btc_av_cb.codec_caps[seid].id = REPO_LDAC_CODEC_ID;
            memset(btc_av_cb.codec_caps[seid].info, 0, AVDT_CODEC_SIZE);
            btc_av_cb.codec_caps[seid].info[0] = 10;
            btc_av_cb.codec_caps[seid].info[2] = REPO_LDAC_CODEC_ID;
            memcpy(btc_av_cb.codec_caps[seid].info + 3, &mcc->cie, 8);
            break;
        }''')
    text = edit_function(text, 'bte_av_media_source_callback', lambda body: replace_once(body,
        '        else {\n            BTC_TRACE_WARNING("bte_av_media_source_callback: unsup_codec',
        '''        else if (repo_ldac_info_valid(p_codec, true)) {
            btc_msg_t msg = { .sig = BTC_SIG_API_CB, .pid = BTC_PID_A2DP, .act = BTC_AV_CONFIG_EVT };
            btc_av_args_t arg;
            memset(&arg, 0, sizeof(arg));
            arg.mcc.type = ESP_A2D_MCT_NON_A2DP;
            memcpy(&arg.mcc.cie, p_codec + 3, 8);
            btc_transfer_context(&msg, &arg, sizeof(arg), NULL, NULL);
        }
        else {
            BTC_TRACE_WARNING("bte_av_media_source_callback: unsup_codec'''))
    text = add_case(text, 'btc_a2d_src_audio_data_send', '''        case REPO_LDAC_CODEC_ID:
            if (!audio_buf->number_frame || audio_buf->number_frame > 15 ||
                audio_buf->data_len + 1 > btc_av_cb.mtu) return BT_STATUS_FAIL;
            buf_offset += 1;
            data_len = audio_buf->data_len;
            break;
''')
    text = add_case(text, 'btc_av_audio_buff_alloc', '''        case REPO_LDAC_CODEC_ID:
            buf_offset += 1;
            break;
''')
    # Both selected-capability and all-capability events must preserve vendor bytes.
    anchor = '                        default: {\n                            BTC_TRACE_WARNING("Unsupported codec type %d", codec_type);'
    text = replace_once(text, anchor, '''                        case REPO_LDAC_CODEC_ID: {
                            param.a2d_report_snk_codec_caps_stat.conn_hdl = btc_av_cb.bta_handle;
                            param.a2d_report_snk_codec_caps_stat.mcc.type = ESP_A2D_MCT_NON_A2DP;
                            memcpy(&param.a2d_report_snk_codec_caps_stat.mcc.cie, codec_caps + 3, 8);
                            btc_a2d_cb_to_app(ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT, &param);
                            break;
                        }
''' + anchor)
    return text


def patch_bta_av_co(text):
    text = re.sub(r'\bBTC_AV_CODEC_NONE\b', 'REPO_BT_CODEC_NONE', text)
    text = add_case(text, 'bta_av_build_src_cfg', '''            case REPO_LDAC_CODEC_ID:
                memset(p_pref_cfg, 0, AVDT_CODEC_SIZE);
                repo_ldac_preferred_config(p_pref_cfg, p_src_cap);
                break;
''')
    text = add_case(text, 'bta_av_co_audio_getconfig', '''    case REPO_LDAC_CODEC_ID:
        supported = repo_ldac_info_known(p_codec_info);
        break;
''')
    text = add_case(text, 'bta_av_co_audio_codec_build_config', '''    case REPO_LDAC_CODEC_ID:
        if (!repo_ldac_config_in_caps(bta_av_co_cb.codec_cfg.info, p_codec_caps)) return FALSE;
        memcpy(p_codec_cfg, bta_av_co_cb.codec_cfg.info, REPO_LDAC_INFO_LEN);
        break;
''')
    text = add_case(text, 'bta_av_co_audio_codec_cfg_matches_caps', '''    case REPO_LDAC_CODEC_ID:
        return repo_ldac_config_in_caps(p_codec_cfg, p_codec_caps);
''')
    text = add_case(text, 'bta_av_co_audio_peer_supports_codec', '''                case REPO_LDAC_CODEC_ID:
                    if (repo_ldac_config_in_caps(bta_av_co_cb.codec_cfg.info, p_peer->snks[index].codec_caps)) {
                        if (p_snk_index) *p_snk_index = index;
                        return TRUE;
                    }
                    break;
''')
    text = add_case(text, 'bta_av_co_audio_media_supports_config', '''    case REPO_LDAC_CODEC_ID:
        for (index = 0; index < BTA_AV_MAX_SEPS; ++index) {
            if (bta_av_co_cb.codec_caps[index].id == REPO_LDAC_CODEC_ID &&
                repo_ldac_config_in_caps(p_codec_cfg, bta_av_co_cb.codec_caps[index].info)) {
                bta_av_co_cb.cur_seid = index;
                bta_av_co_audio_codec_reset();
                return A2D_SUCCESS;
            }
        }
        return A2D_NS_CODEC_TYPE;
''')
    text = add_case(text, 'bta_av_co_audio_setconfig', '''            case REPO_LDAC_CODEC_ID:
                if (codec_type != REPO_LDAC_CODEC_ID) { recfg_needed = TRUE; break; }
                bta_av_co_cb.codec_cfg_setconfig.id = REPO_LDAC_CODEC_ID;
                memcpy(bta_av_co_cb.codec_cfg_setconfig.info, p_codec_info, AVDT_CODEC_SIZE);
                memcpy(bta_av_co_cb.codec_cfg.info, p_codec_info, AVDT_CODEC_SIZE);
                recfg_needed = FALSE;
                break;
''')
    text = add_case(text, 'bta_av_co_fill_sep_mcc_from_sup_snk', '''    case REPO_LDAC_CODEC_ID:
        p_out->mcc.type = ESP_A2D_MCT_NON_A2DP;
        memcpy(&p_out->mcc.cie, p_snk->codec_caps + 3, 8);
        break;
''')
    text = add_case(text, 'bta_av_co_audio_src_data_path', '''        case REPO_LDAC_CODEC_ID:
            *p_timestamp = *((UINT32 *)(p_buf + 1));
            p_buf->offset--;
            p_buf->len++;
            *((UINT8 *)(p_buf + 1) + p_buf->offset) = p_buf->layer_specific & 0x0f;
            break;
''')
    return text


def patch_aact(text):
    text = edit_function(text, 'bta_av_data_path', lambda body: replace_once(body,
        'UINT8   m_pt = 0x60 | p_scb->codec_type;',
        'UINT8   m_pt = p_scb->codec_type == REPO_LDAC_CODEC_ID ? 0x60 : (0x60 | p_scb->codec_type);'))
    text = edit_function(text, 'bta_av_data_path', lambda body: replace_once(body,
        '    if (p_scb->cong) {\n        return;',
        '    if (p_scb->cong) {\n        repo_bt_diag_path(p_scb->l2c_bufs, 1, 0);\n        return;'))
    text = replace_once(text,
        'p_scb->l2c_bufs = (UINT8)L2CA_FlushChannel (p_scb->l2c_cid, L2CAP_FLUSH_CHANS_GET);',
        'p_scb->l2c_bufs = (UINT8)L2CA_FlushChannel (p_scb->l2c_cid, L2CAP_FLUSH_CHANS_GET);\n    repo_bt_diag_path(p_scb->l2c_bufs, 0, 0);')
    text = replace_once(text,
        '            AVDT_WriteReqOpt(p_scb->avdt_handle, p_buf, timestamp, m_pt, opt);',
        '            UINT16 diag_len = p_buf->len;\n'
        '            UINT16 diag_result = AVDT_WriteReqOpt(p_scb->avdt_handle, p_buf, timestamp, m_pt, opt);\n'
        '            repo_bt_diag_write(diag_len, diag_result == AVDT_SUCCESS);')
    return replace_once(text, '                    bta_av_co_audio_drop(p_scb->hndl);',
        '                    repo_bt_diag_path(p_scb->l2c_bufs, 0, 1);\n                    bta_av_co_audio_drop(p_scb->hndl);')


def patch_ext_codec(text):
    def annotate_rejections(body):
        if body.count('        return FALSE;') != 3:
            raise ValueError("External codec enqueue rejection paths changed")
        return body.replace('        return FALSE;',
            '        repo_bt_diag_enqueue(a2dp_source_local_param.audio_tx_q ? fixed_queue_length(a2dp_source_local_param.audio_tx_q) : 0, 0);\n        return FALSE;')
    text = edit_function(text, 'btc_a2dp_source_enqueue_audio_frame', annotate_rejections)
    text = replace_once(text,
        '        APPL_TRACE_WARNING("%s audio tx queue overflow: %d", __func__, fixed_queue_length(a2dp_source_local_param.audio_tx_q));',
        '        /* Rejections are counted below and summarized outside the audio path. */')
    text = replace_once(text,
        '    fixed_queue_enqueue(a2dp_source_local_param.audio_tx_q, p_buf, FIXED_QUEUE_MAX_TIMEOUT);',
        '    fixed_queue_enqueue(a2dp_source_local_param.audio_tx_q, p_buf, FIXED_QUEUE_MAX_TIMEOUT);\n'
        '    repo_bt_diag_enqueue(fixed_queue_length(a2dp_source_local_param.audio_tx_q), 1);')
    text = replace_once(text, '    return fixed_queue_dequeue(a2dp_source_local_param.audio_tx_q, 0);',
        '    BT_HDR *buf = fixed_queue_dequeue(a2dp_source_local_param.audio_tx_q, 0);\n'
        '    repo_bt_diag_read(fixed_queue_length(a2dp_source_local_param.audio_tx_q), buf != NULL);\n    return buf;')
    return replace_once(text, '        osi_free(fixed_queue_dequeue(a2dp_source_local_param.audio_tx_q, 0));',
        '        osi_free(fixed_queue_dequeue(a2dp_source_local_param.audio_tx_q, 0));\n        repo_bt_diag_flush();')


def patch_l2c_link(text):
    # Normal recursion prevention, not a failed send. Keep the return and replace
    # synchronous per-packet UART error output with a cheap diagnostic counter.
    text = replace_once(text, '        L2CAP_TRACE_ERROR("l2cab is_cong_cback_context");',
        '        repo_bt_diag_guard();')
    return edit_function(text, 'l2c_link_process_num_completed_pkts', lambda body: replace_once(body,
        '        p_lcb = l2cu_find_lcb_by_handle (handle);',
        '        p_lcb = l2cu_find_lcb_by_handle (handle);\n'
        '        if (p_lcb) repo_bt_diag_completed(num_sent, p_lcb->sent_not_acked);'))


def patch_avdt_msg(text):
    # Stock Bluedroid caps the peer's discovery list at AVDT_NUM_SEPS, the number of *local*
    # endpoints (CONFIG_BT_A2DP_SEP_NUM_MAX, at most 3). A sink that lists sources or vendor
    # endpoints first (for example a JBL PartyBox: source, aptX-family sink, source, then SBC and
    # AAC sinks) loses its usable sinks and the stream open fails. The line above the removed
    # check already bounds the count by the caller's buffer (BTA_AV_NUM_SEPS entries).
    return edit_function(text, 'avdt_msg_prs_discover_rsp', lambda body: replace_once(body,
        '    if (p_msg->discover_rsp.num_seps > AVDT_NUM_SEPS) {\n'
        '        p_msg->discover_rsp.num_seps = AVDT_NUM_SEPS;\n'
        '    }\n',
        '    /* Project overlay: bounded by the caller-supplied buffer above, not by local SEPs. */\n'))


def check_discovery_buffer(aact_text, av_int_text):
    """Fail closed unless every AVDTP discovery still lands in a BTA_AV_NUM_SEPS-entry buffer.

    patch_avdt_msg is only memory-safe because the caller's buffer bounds the parsed count.
    """
    call = 'AVDT_DiscoverReq(p_scb->peer_addr, p_scb->sep_info, BTA_AV_NUM_SEPS,'
    total = aact_text.count('AVDT_DiscoverReq(')
    if total == 0 or aact_text.count(call) != total:
        raise ValueError("AVDTP discovery callers changed; re-check the patch_avdt_msg bound")
    if 'tAVDT_SEP_INFO      sep_info[BTA_AV_NUM_SEPS];' not in av_int_text:
        raise ValueError("BTA AV discovery buffer changed; re-check the patch_avdt_msg bound")


PATCHERS = dict(zip(SOURCES, (patch_btc_av, patch_bta_av_co, patch_aact, patch_ext_codec, patch_l2c_link,
                              patch_avdt_msg)))


def generate(framework, output):
    framework, output = Path(framework), Path(output)
    check_discovery_buffer(source_path(framework, 'bta_av_aact.c').read_text(encoding='utf-8'),
                           (framework / 'components/bt/host/bluedroid/bta/av/include/bta_av_int.h')
                           .read_text(encoding='utf-8'))
    sources = {name: '#include "repo_ldac_transport.h"\n#include "repo_bt_diag.h"\n' + patcher(
        source_path(framework, name).read_text(encoding='utf-8')) for name, patcher in PATCHERS.items()}
    api = framework / 'components/bt/host/bluedroid/api/include/api/esp_a2dp_api.h'
    sources['include/esp_a2dp_api.h'] = replace_once(api.read_text(encoding='utf-8'), '    } cie;',
        '        uint8_t vendor_info[8];             /*!< Project LDAC capability bytes */\n    } cie;')
    for name in ('repo_ldac_transport.h', 'repo_bt_diag.h', 'repo_bt_diag.c'):
        sources[name] = Path(__file__).with_name(name).read_text()
    for name, text in sources.items():
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_text(encoding='utf-8') != text:
            target.write_text(text, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    generate(args.framework, args.output)
