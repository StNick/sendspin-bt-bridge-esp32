"""Checked source transformation and vendor transport regression tests."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("overlay", ROOT / "scripts/prepare_ldac_transport.py")
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)


class OverlayTests(unittest.TestCase):
    def test_changed_or_ambiguous_sdk_anchor_fails(self):
        for text in ("missing", "anchor anchor"):
            with self.assertRaises(ValueError):
                overlay.replace_once(text, "anchor", "replacement")

    def test_pointer_return_and_quoted_braces(self):
        source = '\nvoid caller(void) { target(); }\nvoid *target(void) { /* } */ const char *s = "{"; return 0; }\n'
        start, end = overlay.function_span(source, "target")
        self.assertEqual(source[start:end], '{ /* } */ const char *s = "{"; return 0; }')

    def test_instrumentation_preserves_flow_and_quiets_only_normal_guard(self):
        framework = Path(os.environ.get("IDF_PATH") or Path.home() / ".platformio/packages/framework-espidf")
        l2c = overlay.patch_l2c_link(overlay.source_path(framework, "l2c_link.c").read_text())
        self.assertIn('if (l2cb.is_cong_cback_context) {\n        repo_bt_diag_guard();', l2c)
        self.assertIn('repo_bt_diag_guard();\n        return;', l2c)
        self.assertNotIn('L2CAP_TRACE_ERROR("l2cab is_cong_cback_context")', l2c)
        self.assertIn('L2CAP_TRACE_ERROR ("l2c_link_process_num_completed_pkts: evt too short', l2c)
        self.assertIn('repo_bt_diag_completed(num_sent, p_lcb->sent_not_acked)', l2c)
        ext = overlay.patch_ext_codec(overlay.source_path(framework, "btc_a2dp_source_ext_codec.c").read_text())
        self.assertEqual(ext.count('repo_bt_diag_enqueue('), 4)
        self.assertIn('repo_bt_diag_read(', ext)
        self.assertIn('repo_bt_diag_flush();', ext)
        self.assertIn('#define MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ     (27)', ext)
        changed = overlay.source_path(framework, "btc_a2dp_source_ext_codec.c").read_text().replace(
            '        return FALSE;', '        return 0;', 1)
        with self.assertRaises(ValueError):
            overlay.patch_ext_codec(changed)

    def test_discovery_keeps_every_peer_endpoint(self):
        # A JBL PartyBox lists six endpoints with its SBC/AAC sinks last; the stock cap at the
        # local SEP count (3) dropped them. The caller-buffer bound must remain.
        framework = Path(os.environ.get("IDF_PATH") or Path.home() / ".platformio/packages/framework-espidf")
        avdt = overlay.patch_avdt_msg(overlay.source_path(framework, "avdt_msg.c").read_text())
        start, end = overlay.function_span(avdt, "avdt_msg_prs_discover_rsp")
        body = avdt[start:end]
        self.assertNotIn("AVDT_NUM_SEPS", body)
        self.assertIn("if (p_msg->discover_rsp.num_seps > (len / 2)) {", body)
        self.assertIn("AVDT_SEID_MAX", body)
        with self.assertRaises(ValueError):
            overlay.patch_avdt_msg(avdt)
        aact = overlay.source_path(framework, "bta_av_aact.c").read_text()
        av_int = (framework / "components/bt/host/bluedroid/bta/av/include/bta_av_int.h").read_text()
        overlay.check_discovery_buffer(aact, av_int)
        with self.assertRaises(ValueError):  # a caller with a smaller buffer
            overlay.check_discovery_buffer(aact.replace("p_scb->sep_info, BTA_AV_NUM_SEPS", "p_scb->sep_info, 3", 1), av_int)
        with self.assertRaises(ValueError):  # the buffer itself shrank
            overlay.check_discovery_buffer(aact, av_int.replace("sep_info[BTA_AV_NUM_SEPS]", "sep_info[4]"))

    def test_repeatable_generation_preserves_shared_sdk(self):
        framework = Path(os.environ.get("IDF_PATH") or Path.home() / ".platformio/packages/framework-espidf")
        original = {name: overlay.source_path(framework, name).read_bytes() for name in overlay.SOURCES}
        with tempfile.TemporaryDirectory() as temp:
            dest = Path(temp)
            overlay.generate(framework, dest)
            first = {str(p.relative_to(dest)): p.read_bytes() for p in dest.rglob("*") if p.is_file()}
            overlay.generate(framework, dest)
            self.assertEqual(first, {str(p.relative_to(dest)): p.read_bytes() for p in dest.rglob("*") if p.is_file()})
            self.assertIn("uint8_t vendor_info[8]", (dest / "include/esp_a2dp_api.h").read_text())
            self.assertIn("? 0x60", (dest / "bta_av_aact.c").read_text())
            for name in overlay.SOURCES:
                self.assertEqual(original[name], overlay.source_path(framework, name).read_bytes())
                self.assertIn(b"repo_ldac_transport.h", (dest / name).read_bytes())
            for name in ("btc_av.c", "bta_av_co.c"):
                self.assertNotIn(b"BTC_AV_CODEC_NONE", (dest / name).read_bytes())
            co = (dest / "bta_av_co.c").read_text()
            for function in ("bta_av_co_audio_getconfig", "bta_av_co_audio_codec_build_config",
                             "bta_av_co_audio_media_supports_config", "bta_av_co_audio_peer_supports_codec"):
                start, end = overlay.function_span(co, function)
                self.assertIn("repo_ldac_", co[start:end])


if __name__ == "__main__":
    unittest.main()
