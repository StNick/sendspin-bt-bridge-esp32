"""Adapt the receiver project's checked SDK overlay for LDAC transmission.

Generate private copies; never modify the shared ESP-IDF package. Every SDK
anchor must match exactly or configuration fails before writing any output.
"""
import argparse
from pathlib import Path
import re

SOURCES = ("btc_av.c", "bta_av_co.c", "bta_av_aact.c")
SOURCE_DIR = "components/bt/host/bluedroid/btc/profile/std/a2dp"


def source_path(framework, name):
    directory = SOURCE_DIR if name != "bta_av_aact.c" else "components/bt/host/bluedroid/bta/av"
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
    return edit_function(text, 'bta_av_data_path', lambda body: replace_once(body,
        'UINT8   m_pt = 0x60 | p_scb->codec_type;',
        'UINT8   m_pt = p_scb->codec_type == REPO_LDAC_CODEC_ID ? 0x60 : (0x60 | p_scb->codec_type);'))


PATCHERS = dict(zip(SOURCES, (patch_btc_av, patch_bta_av_co, patch_aact)))


def generate(framework, output):
    framework, output = Path(framework), Path(output)
    sources = {name: '#include "repo_ldac_transport.h"\n' + patcher(
        source_path(framework, name).read_text(encoding='utf-8')) for name, patcher in PATCHERS.items()}
    api = framework / 'components/bt/host/bluedroid/api/include/api/esp_a2dp_api.h'
    sources['include/esp_a2dp_api.h'] = replace_once(api.read_text(encoding='utf-8'), '    } cie;',
        '        uint8_t vendor_info[8];             /*!< Project LDAC capability bytes */\n    } cie;')
    sources['repo_ldac_transport.h'] = Path(__file__).with_name('repo_ldac_transport.h').read_text()
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
