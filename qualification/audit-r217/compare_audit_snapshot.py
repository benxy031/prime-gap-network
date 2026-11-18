"""Compare complete audit RPC outputs and times through the same accepted-block path."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import statistics
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tests'))
from native_grouped249_actual import GENESIS, Node


def file_hash(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def unpack(archive, directory):
    assert file_hash(archive) == 'dd55d578876fd0ccc9b1a8f07f6b28b3393a96f0515d4858e117931c5b7186e6'
    with zipfile.ZipFile(archive) as bundle:
        fixture = json.loads(bundle.read('BENCH-FIXTURE.json'))
        assert fixture['genesis'] == GENESIS and fixture['public_data_only']
        assert fixture['start_height'] == 5681 and len(fixture['next_blocks']) == 16
        assert set(bundle.namelist()) == {'BENCH-FIXTURE.json'} | set(fixture['files'])
        assert sum(item.file_size for item in bundle.infolist()) < 48 * 1024**2
        for relative, fingerprint in fixture['files'].items():
            path = PurePosixPath(relative)
            assert path.parts[0] == 'testnet' and path.parts[1] in (
                'blocks', 'chainstate', 'chainstate-pgn-request-history')
            assert '..' not in path.parts and chr(92) not in relative and ':' not in relative
            raw = bundle.read(relative)
            assert hashlib.sha256(raw).hexdigest() == fingerprint
            output = directory / path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(raw)
    return fixture


def observe(node):
    start = time.perf_counter_ns()
    output = node.rpc('getpgnauditactivity', [0, 12])
    elapsed = (time.perf_counter_ns() - start) / 1e6
    assert output['schema'] == 'PGN_AUDIT_ACTIVITY_V1'
    return output, elapsed


def measure(executable, archive):
    with tempfile.TemporaryDirectory(prefix='pgn-audit-pair-') as folder:
        directory = Path(folder)
        fixture = unpack(archive, directory)
        node = Node(executable, directory)
        try:
            node.rpc('logging', [['rpc'], []])
            assert node.rpc('getblockhash', [5681]) == fixture['start_hash']
            values = [observe(node)[0]]
            first, warm = [], []
            for block in fixture['next_blocks']:
                assert node.rpc('submitblock', [block['raw']]) is None
                value, elapsed = observe(node)
                assert value['state']['height'] == block['height']
                assert value['state']['bestblockhash'] == block['hash']
                values.append(value)
                first.append(elapsed)
                for _ in range(3):
                    repeated, elapsed = observe(node)
                    assert repeated == value
                    warm.append(elapsed)
            block_hash = fixture['next_blocks'][-1]['hash']
            node.rpc('invalidateblock', [block_hash])
            assert observe(node)[0] == values[-2]
            node.rpc('reconsiderblock', [block_hash])
            assert observe(node)[0] == values[-1]
        finally:
            node.close()
        debug = directory / 'testnet/debug.log'
        assert debug.stat().st_size < 6 * 1024**2
        with debug.open('rb') as stream:
            cached = {line.strip().split()[-1].decode() for line in stream
                      if b'PGN RPC audit snapshot direct-child' in line}
        assert {block['hash'] for block in fixture['next_blocks']} <= cached
        return values, first, warm


def main():
    parser = argparse.ArgumentParser()
    for name in ('baseline', 'indexed', 'fixture', 'receipt'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    executables = {
        'baseline': args.baseline.resolve(strict=True),
        'indexed': args.indexed.resolve(strict=True),
    }
    hashes = {key: file_hash(path) for key, path in executables.items()}
    assert hashes['baseline'] != hashes['indexed']
    results = None
    first = {key: [] for key in executables}
    warm = {key: [] for key in executables}
    for order in (('baseline', 'indexed'), ('indexed', 'baseline')):
        for key in order:
            values, direct, repeated = measure(executables[key], args.fixture)
            if results is None:
                results = values
            else:
                assert values == results
            first[key].extend(direct)
            warm[key].extend(repeated)
    medians = {key: statistics.median(row) for key, row in first.items()}
    report = dict(PASS=True, exact_projection=True, restored_tip=True, executables=hashes,
                  direct_samples_per_binary=len(first['indexed']), direct_median_ms=medians,
                  indexed_delta_percent=(medians['indexed']-medians['baseline'])*100/medians['baseline'],
                  same_tip_median_ms={key: statistics.median(row) for key, row in warm.items()},
                  scope='Verified h5681–5697 from the same snapshot, paired order, offline, no wallet.')
    args.receipt.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
