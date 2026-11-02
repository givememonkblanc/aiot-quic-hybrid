"""Compile original imported C selectors, using the local picoquic headers only."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--picoquic', type=Path, default=Path('/home/ryzen395/mpquic/third_party/picoquic'))
    args = parser.parse_args()
    client = ROOT / 'reference/src/client'
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / 'probe'
        command = ['cc', '-std=gnu11', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
                   '-fsanitize=address,undefined', '-Wl,--gc-sections']
        for part in ('loglib', 'picoquic', 'c4/src', 'picoquic_mbedtls'):
            command += ['-I', str(args.picoquic / part)]
        command += [str(ROOT / 'probe.c'), str(client / 'client_paths.c')]
        command += [str(p) for p in sorted((client / 'scheduler').glob('*.c'))]
        subprocess.run(command + ['-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
        (ROOT / 'results/decision_probe.csv').write_text(result.stdout)
        print(result.stdout, end='')


if __name__ == '__main__':
    main()
