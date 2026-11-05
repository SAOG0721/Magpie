"""Run the actual-template probe in its own portable directory, never the product."""
from pathlib import Path
import json
import os
import subprocess
import sys

probe = Path(sys.argv[1]).resolve()
assert (probe / 'parameter-flyout-probe.marker').is_file()
output = probe / 'checkbox-results'
output.mkdir(exist_ok=False)
sections = ['Basic', 'Tone', 'Color', 'Mixer', 'Wheels', 'Selection', 'Style',
            'Spatial', 'Curves', 'Mask', 'Automatic', 'Calibration', 'Output', 'Lut']
cases = [('OKLabImageAdjustment', i, label) for i, label in enumerate(sections)]
cases += [(name, 0, name.split('\\')[-1]) for name in [
    'NvidiaVFX\\VFG', 'CRT\\CRT_Easymode', 'CRT\\CRT_Geom', 'CRT\\CRT_Hyllian',
    'CRT\\GTU_v050', 'RTXVideo\\RTXVideo_HDR', 'DLSSNR\\DLSSNR_AI_Filter']]
results = []
for effect, section, label in cases:
    case = f'{len(results):02}-{label}'
    config = dict(alwaysRunAsAdmin=False, showNotifyIcon=False, autoCheckForUpdates=False,
                  adminStartupDefaultsVersion=1, experimentalOpticalFlowDefaultsVersion=1,
                  scalingModes=[dict(name='Checkbox regression', effects=[dict(name=effect,
                      scalingType=1, scale=dict(x=1, y=1), parameters={})])], profiles=[])
    (probe / 'config/v4e/config.json').write_text(json.dumps(config), encoding='utf-8')
    log = probe / 'logs/magpie.log'
    if log.exists():
        log.write_text('', encoding='utf-8')
    env = os.environ.copy()
    env['MAGPIE_DIAG_SECTION'] = str(section)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    child = subprocess.Popen([str(probe / 'Magpie.exe')], cwd=probe, env=env, startupinfo=startup)
    try:
        exit_code = child.wait(timeout=35)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()
        raise RuntimeError(f'{case}: isolated child timed out')
    text = log.read_text(encoding='utf-8-sig', errors='replace') if log.exists() else ''
    (output / f'{case}.log').write_text(text, encoding='utf-8')
    passed = exit_code == 0 and 'CheckboxProbe PASS' in text and 'opens=2' in text
    results.append(dict(case=case, effect=effect, section=section, exitCode=exit_code, passed=passed))
    (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(json.dumps(results[-1]), flush=True)
    if not passed:
        raise RuntimeError(f'{case}: probe failed; inspect {output}')
print('PASS 21 actual-template cases, reopen, checkbox edits, both slots and all curves.')
