"""Build/flash a bounded PI candidate, then measure 6 A and 10 A on COM6."""
import argparse
import json
import re
import subprocess
from pathlib import Path
from analyze_pi_tuning import analyze

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('kp', type=float)
p.add_argument('ki', type=float)
p.add_argument('--tag', required=True)
p.add_argument('--slew', type=float, default=15)
args = p.parse_args()
if not (0.05 <= args.kp <= 0.8 and 1 <= args.ki <= 160 and 15 <= args.slew <= 60):
    p.error('Candidate outside experiment bounds')
if not re.fullmatch(r'[a-zA-Z0-9_]+', args.tag):
    p.error('Invalid tag')
root = Path(__file__).resolve().parents[1]
bundle = Path('C:/Users/kerok/AppData/Local/stm32cube/bundles')
config = root / 'Core/Inc/motor_control_config.h'
text = config.read_text(encoding='utf-8')
for key, value in [('KP_D', args.kp), ('KP_Q', args.kp), ('KI_D', args.ki),
                   ('KI_Q', args.ki), ('SLEW_A_PER_SEC', args.slew)]:
    text, count = re.subn(r'(#define MOTOR_CONTROL_CURRENT_' + key + r'\s+)[\d.]+f',
                          lambda m: m[1] + str(float(value)) + 'f', text)
    assert count == 1, key
config.write_text(text, encoding='utf-8')

def run(command, suffix):
    result = subprocess.run(list(map(str, command)), cwd=root, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    (root / f'build/pi_sweep_{args.tag}_{suffix}.txt').write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError(f'{suffix} failed; see build/pi_sweep_{args.tag}_{suffix}.txt')
    return result.stdout

run([bundle / 'cmake/4.0.1+st.3/bin/cmake.exe', '--build', '--preset', 'Debug'], 'build')
flash = run([bundle / 'programmer/2.23.0/bin/STM32_Programmer_CLI.exe', '-c',
             'port=SWD', 'mode=UR', '-d', 'build/Debug/HiradoraGen3FW.elf', '-v', '-rst'], 'flash')
assert b'Download verified successfully' in flash
results = []
for iq in (6, 10):
    log = f'build/pi_sweep_{args.tag}_{iq}a.log'
    run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
         'tools/measure_current_pi.ps1', '-Iq', iq, '-DurationMs', 1800, '-Log', log], f'{iq}a_console')
    result = analyze(root / log)
    results.append(result)
    print(json.dumps(dict(kp=args.kp, ki=args.ki, slew=args.slew, **result)), flush=True)
(root / f'build/pi_sweep_{args.tag}.json').write_text(json.dumps(results, indent=2))
