"""Run immutable public AI COLUMN SQL/results using the bundled mysqltest.

Requires a dedicated disposable socket-only seekdb instance. Original mock bytes
are retained; a launcher relocates only its temporary state to the result tree.
No model request/response, SQL, assertions or expected results are rewritten.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import sys

CASES = ('ai_column_ddl', 'ai_column_t1_dml', 'ai_column_worker',
         'ai_column_retry_strict', 'ai_column_vector_index')
PREFIX = 'tools/deploy/mysql_test/test_suite/ai_column/'
FIXTURE = PREFIX + 'include/ai_column_mock.py'


def original(source, relative):
    return subprocess.check_output(['git', '-C', str(source), 'show', 'HEAD:' + relative])


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(args):
    source = args.source_root.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {'source_commit': subprocess.check_output(
        ['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip(),
        'transport_adaptation': 'Mock state directory only; original fixture executed via launcher',
        'official_platform_score': False, 'cases': []}
    fixture_bytes = original(source, FIXTURE)
    for name in args.case or CASES:
        work = output / name
        work.mkdir()
        paths = {}
        inputs = {}
        for suffix, relative in (('test', PREFIX + 't/' + name + '.test'),
                                 ('result', PREFIX + 'r/mysql/' + name + '.result'),
                                 ('fixture', FIXTURE)):
            data = fixture_bytes if suffix == 'fixture' else original(source, relative)
            target = work / (name + '.' + suffix)
            target.write_bytes(data)
            paths[suffix] = target
            inputs[suffix] = {'source': relative, 'sha256': digest(data)}
        # Preserve the fixture's behavior, including child invocations, while
        # ensuring all pid/log state belongs to this fresh test directory.
        launcher = work / 'mysql_test/test_suite/ai_column/include/ai_column_mock.py'
        launcher.parent.mkdir(parents=True)
        state = work / 'mock-state'
        state.mkdir()
        launcher.write_text(
            'from pathlib import Path\n'
            'namespace={"__name__":"_original_mock_","__file__":__file__}\n'
            f'original=Path({str(paths["fixture"])!r})\n'
            'exec(compile(original.read_bytes(),str(original),"exec"),namespace)\n'
            f'namespace["state_dir"]=lambda port:str(Path({str(state)!r})/str(port))\n'
            'namespace["main"]()\n', encoding='utf-8')
        ports = sorted(set(int(p) for p in re.findall(
            rb'ai_column_mock\.py start ([0-9]+)', paths['test'].read_bytes())))
        # Never stop another process just because it occupies an official port.
        for port in ports:
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', port))
        command = [str(args.mysqltest.resolve()), '--no-defaults', '--user=root',
                   '--socket=' + args.socket, '--database=test',
                   '--default-character-set=utf8mb4', '--connect-timeout=10',
                   '--max-connect-retries=1', '--logdir=' + str(work),
                   '--test-file=' + str(paths['test']), '--result-file=' + str(paths['result'])]
        code = None
        try:
            result = subprocess.run(command, cwd=work, capture_output=True,
                                    text=True, timeout=args.timeout)
            code = result.returncode
            (work / 'stdout.log').write_text(result.stdout, encoding='utf-8')
            (work / 'stderr.log').write_text(result.stderr, encoding='utf-8')
            print(f'{name}: exit {code}\n{result.stdout}{result.stderr}', flush=True)
        finally:
            for port in ports:
                subprocess.run([sys.executable, str(launcher), 'stop', str(port)],
                               cwd=work, check=True, timeout=15)
            for suffix, target in paths.items():
                if digest(target.read_bytes()) != inputs[suffix]['sha256']:
                    raise RuntimeError('Public acceptance input changed: ' + suffix)
            report['cases'].append({'name': name, 'returncode': code,
                                    'inputs': inputs, 'argv': command})
            report['all_passed'] = all(c['returncode'] == 0 for c in report['cases'])
            (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return 0 if report['all_passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('--mysqltest', required=True, type=Path)
    parser.add_argument('--socket', required=True)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--timeout', type=int, default=300)
    return run(parser.parse_args())


if __name__ == '__main__':
    raise SystemExit(main())
