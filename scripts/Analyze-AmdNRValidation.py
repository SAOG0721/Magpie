#!/usr/bin/env python3
"""Summarize local AMD NR evidence; displayed FG frames require PresentMon ETW data."""
import argparse
import csv
import importlib.util
import json
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path


def summary(values):
    values = sorted(values)
    if not values:
        return None
    return dict(samples=len(values), median_ms=statistics.median(values),
                p95_ms=values[min(len(values)-1, int(len(values)*.95))],
                maximum_ms=max(values))


def analyze(directory):
    log = directory / 'magpie-combined.log'
    if not log.exists():
        text = '\n'.join(p.read_text(encoding='utf-8-sig') for p in sorted((directory/'runtime/logs').glob('magpie*.log')))
    else:
        text = log.read_text(encoding='utf-8-sig')
    report = dict(directory=str(directory.resolve()), spatial_pipeline='640x360 WGC -> 640x360 AMD NR -> 1280x720 SDK SR',
                  limits=['Artificial source, not real game quality or long-term stability.',
                          'Stage timings are synchronized wall time or CPU envelopes, not exclusive GPU busy time.',
                          'NR warm-up passthrough is excluded from steady-state processing statistics.',
                          'WGC cadence is observed dequeuing, not the game engine frame rate.',
                          'Displayed frame counts may include repeats; generated frames require ETW FrameType.',
                          'XeSS GPU interpolation cost is not isolated.'])
    nr = re.findall(r'sequence=(\d+) synchronous_total_ms=([\d.]+)', text)
    report['nr_warmup_wall'] = summary([float(ms) for seq, ms in nr if int(seq)==1])
    report['nr_sampled_wall'] = summary([float(ms) for seq, ms in nr if int(seq)>1])
    report['sr_sampled_sync_wall'] = summary([float(v) for v in re.findall(r'FSR dispatch completed:[^\n]*synchronized_wall_ms=([\d.]+)', text)])
    report['providers'] = sorted(set(re.findall(r'FSR dispatch completed: provider=(\d+) name=([^ ]+)', text)))
    report['nr_pixel_differences_255'] = [float(v) for v in re.findall(r'backend=AMDNR[^\n]*meanAbsoluteDifference=([\d.]+)/255', text)]
    report['of_sync_wall'] = summary([float(v) for v in re.findall(r'AMD OF synchronous validation:[^\n]*synchronized_wall_ms=([\d.]+)', text)])
    report['fg_cpu'] = {key: summary([float(v) for v in re.findall(key+r'=([\d.]+)', text)])
                        for key in ['tag_cpu_ms', 'XeLL_cpu_wait_ms', 'proxy_present_cpu_ms']}
    spec = importlib.util.spec_from_file_location('frame_trace', Path(__file__).with_name('Analyze-FrameTrace.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    traces = []
    for path in sorted((directory/'runtime/logs/frame-traces').glob('trace*.csv')):
        meta, rows = module.read_trace(path)
        unique = {(r['thread'],r['event'],r['start_us'],r['duration_us'],r['frame_id'],r['a'],r['b']):r for r in rows}
        rows = sorted(unique.values(), key=lambda r:r['start_us'])
        item = dict(file=path.name, duration_seconds=meta['duration_us']/1e6,
                    events=dict(Counter(r['event'] for r in rows)), ring_overwritten=any(v['overwritten'] for v in meta['lanes'].values()))
        for event in ['WgcFrame','CaptureAccepted','ContentSubmit','OverlaySubmit']:
            stamps = [r['start_us'] for r in rows if r['event']==event and r['frame_id']>=4]
            item[event+'_observed_hz'] = (len(stamps)-1)*1e6/(stamps[-1]-stamps[0]) if len(stamps)>1 else None
        for index, label in [(0,'NR'),(1,'SR')]:
            selected = [r for r in rows if r['event']=='NativeEffect' and r['a']==index and r['frame_id']>=4]
            item[label+'_cpu_envelope'] = summary([r['duration_us']/1000 for r in selected])
            stamps = [r['start_us']+r['duration_us'] for r in selected]
            item[label+'_processed_hz'] = (len(stamps)-1)*1e6/(stamps[-1]-stamps[0]) if len(stamps)>1 else None
        item['present_cpu_envelope'] = summary([r['duration_us']/1000 for r in rows if r['event']=='Present' and r['frame_id']>=4])
        traces.append(item)
    report['traces'] = traces
    groups = defaultdict(list)
    pm = directory/'presentmon.csv'
    if pm.exists():
        with pm.open(encoding='utf-8-sig', newline='') as file:
            for row in csv.DictReader(file):
                if row['PresentRuntime']=='DXGI' and row['SwapChainAddress']!='0x0':
                    groups[row['SwapChainAddress']].append(row)
    chains = []
    for address, rows in groups.items():
        shown = [r for r in rows if r['DisplayLatency']!='NA' and r['DisplayedTime']!='NA']
        types = Counter(r['FrameType'] for r in shown)
        stamps = sorted(float(r['CPUStartTime'])+float(r['DisplayLatency']) for r in shown)
        chains.append(dict(address=address, submitted_events=len(rows), actually_displayed_events=len(shown),
                           displayed_types=dict(types), present_modes=dict(Counter(r['PresentMode'] for r in shown)),
                           displayed_interval=summary([float(r['DisplayedTime']) for r in shown]),
                           observed_display_events_hz=(len(stamps)-1)*1000/(stamps[-1]-stamps[0]) if len(stamps)>1 and stamps[-1]>stamps[0] else None))
    report['presentation_chains'] = chains
    qualifying = [c for c in chains if c['displayed_types'].get('Intel XeSS-FG',0)>=100
                  and c['displayed_types'].get('Application',0)>=100
                  and .8 <= c['displayed_types']['Intel XeSS-FG']/c['displayed_types']['Application'] <= 1.2]
    report['fg2_display_evidence_pass'] = bool(qualifying and 'lastFGResult=0' in text
        and any(v>0 for v in report['nr_pixel_differences_255'])
        and any(name=='3.1.5' for _,name in report['providers']))
    return report


if __name__=='__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--require-fg2',action='store_true')
    args = parser.parse_args()
    report = analyze(args.directory)
    output = args.directory/'analysis.json'
    output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'Evidence report: {output}; actual displayed FG2 pass={report["fg2_display_evidence_pass"]}')
    if args.require_fg2 and not report['fg2_display_evidence_pass']:
        raise SystemExit('FG2 acceptance requires actually displayed Application and Intel XeSS-FG events.')
