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
