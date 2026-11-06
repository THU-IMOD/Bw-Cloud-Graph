#!/usr/bin/env python3
"""Sequential BYO comparisons on existing stores; preserves failures and raw samples."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--build-dir', type=Path, default=Path('build'),
                   help='Build tree containing the Release byo_bench executable')
    p.add_argument('--configs', type=Path, nargs='+', required=True)
    p.add_argument('--budgets', type=float, nargs='+', default=[64, 32, 16, 8, 4])
    p.add_argument('--modes', nargs='+', choices=['old', 'reference', 'byo', 'acgraph', 'acgraph-sync', 'hybrid', 'page', 'page-io', 'page-packed'], default=['old', 'byo'])
    p.add_argument('--algorithms', nargs='+', choices=['bfs', 'cc', 'pagerank'], default=['bfs', 'cc', 'pagerank'])
    p.add_argument('--rounds', type=int, default=3)
    p.add_argument('--threads', type=int, default=15)
    p.add_argument('--iterations', type=int, default=20)
    p.add_argument('--charge-auxiliary', action='store_true')
    p.add_argument('--distance-window', type=int, default=8)
    p.add_argument('--timeout', type=int, default=120)
    p.add_argument('--continue-on-error', action='store_true')
    a = p.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = (root / a.build_dir).resolve()
    binary = build / 'bw-test/byo_bench'
    cache = (build / 'CMakeCache.txt').read_text()
    if 'CMAKE_BUILD_TYPE:STRING=Release' not in cache:
        raise SystemExit('Performance measurements require a Release build')
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    files = subprocess.check_output(['git', 'ls-files', '-co', '--exclude-standard', '-z'], cwd=root).split(b'\0')
    metadata = {'host': platform.uname()._asdict(), 'binary_sha256': sha(binary),
                'command': sys.argv, 'sources': {x.decode(): sha(root / x.decode()) for x in set(files)
                if x and (root / x.decode()).is_file() and not x.endswith(b'.pyc')},
                'compiler': subprocess.check_output(['c++', '--version'], text=True), 'configs': {}, 'inputs': {}}
    for config in a.configs:
        metadata['configs'][str(config.resolve())] = config.read_text()
        # Effective configs from benchmark_smo name their full input graph explicitly.
        import re
        name = re.search(r'^  name:\s*["\']?([\w.]+)', config.read_text(), re.M).group(1)
        graph = root / 'data' / (name + '.graph')
        metadata['inputs'][str(graph)] = sha(graph)
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2))
    (out / 'CMakeCache.txt').write_bytes((build / 'CMakeCache.txt').read_bytes())
    records = []
    failed = False
    for budget in a.budgets:
        for config in a.configs:
            for algorithm in a.algorithms:
                for mode in a.modes:
                    name = f'{config.stem}-{algorithm}-{mode}-{budget:g}'
                    command = [str(binary), str(root), str(config.resolve()), str(budget), str(a.rounds),
                               str(a.threads), mode, str(out / (name + '.json')), algorithm, str(a.iterations), str(a.distance_window),
                               "charge" if a.charge_auxiliary else "frames"]
                    with (out / (name + '.log')).open('w') as log:
                        try:
                            code = subprocess.run(command, cwd=root, stdout=log,
                                                  stderr=subprocess.STDOUT, timeout=a.timeout).returncode
                        except subprocess.TimeoutExpired:
                            log.write('\n[fail] timeout\n')
                            code = 124
                    records.append({'command': command, 'returncode': code})
                    (out / 'commands.json').write_text(json.dumps(records, indent=2))
                    print((out / (name + '.log')).read_text().strip(), flush=True)
                    failed |= code != 0
                    if code and not a.continue_on_error:
                        return 1
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
