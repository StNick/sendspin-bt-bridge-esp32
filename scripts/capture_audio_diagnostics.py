"""Capture low-rate, timestamped audio/transport status and logs from a bridge."""
import argparse
import json
import math
import os
from pathlib import Path
import time
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('host')
parser.add_argument('--seconds', type=float, default=90)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--username', default='admin')
args = parser.parse_args()
if not math.isfinite(args.seconds) or args.seconds <= 0:
    parser.error('--seconds must be a positive finite duration')
base = args.host if '://' in args.host else 'http://' + args.host
args.output.parent.mkdir(parents=True, exist_ok=True)
credentials = urllib.request.HTTPPasswordMgrWithDefaultRealm()
password = os.environ.get('SENDSPIN_WEB_PASSWORD')
if password is not None:
    credentials.add_password(None, base, args.username, password)
opener = urllib.request.build_opener(urllib.request.HTTPBasicAuthHandler(credentials))

def request(path):
    with opener.open(base.rstrip('/') + path, timeout=5) as response:
        return response.read()

start = time.monotonic()
last_report = -15
with args.output.with_suffix('.jsonl').open('w', encoding='utf-8') as out:
    while time.monotonic() - start < args.seconds:
        tick = time.monotonic()
        try:
            status = json.loads(request('/api/status'))
            bt, ss = status['bluetooth'], status['sendspin']
            sample = {'elapsed_s': round(tick - start, 3), 'uptime_s': status['system']['uptime_s'],
                      'wifi_rssi': status['wifi']['rssi'],
                      'playing': ss['playback_state'], 'stream_codec': ss['stream_codec'],
                      'input_rate': ss.get('sample_rate'), 'input_bits': ss.get('bit_depth'),
                      **{k: v for k, v in bt.items() if k in (
                          'codec', 'bitrate_bps', 'encoder_avg_us', 'encoder_max_us', 'encoder_late_batches',
                          'send_retries', 'dropped_packets', 'pcm_buffer_ms', 'pcm_underrun_frames', 'transport')}}
            out.write(json.dumps(sample) + '\n')
            out.flush()
            if tick - start - last_report >= 15:
                last_report = tick - start
                print(json.dumps(sample), flush=True)
        except Exception as exc:
            out.write(json.dumps({'elapsed_s': round(tick - start, 3), 'error': str(exc)}) + '\n')
            print(str(exc), flush=True)
        time.sleep(max(0, 1 - (time.monotonic() - tick)))
try:
    args.output.with_suffix('.log').write_bytes(request('/api/logs'))
except OSError as exc:
    parser.exit(1, f'Status samples saved, but fetching the final log failed: {exc}\n')
