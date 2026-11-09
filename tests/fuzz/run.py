#!/usr/bin/env python3
"""Bounded, fail-closed offline corpus replay / coverage campaign."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
p = argparse.ArgumentParser()
p.add_argument('build', type=Path)
p.add_argument('--campaign', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parent
for name in ('parser', 'history', 'log'):
    binary = a.build.resolve() / ('fuzz-' + name)
    seeds = sorted((root / 'corpus' / name).iterdir())
    if not seeds or not binary.is_file():
        raise SystemExit('Missing fuzz binary or seed corpus: ' + name)
    # Never let libFuzzer mutate the reviewed source corpus.
    with tempfile.TemporaryDirectory(prefix='nero-fuzz-') as temp:
        corpus = Path(temp) / 'corpus'
        shutil.copytree(root / 'corpus' / name, corpus)
        if a.campaign:
            args = [str(binary), str(corpus), '-seed=1905', '-runs=10000',
                    '-max_total_time=30', '-timeout=2', '-rss_limit_mb=512',
                    '-malloc_limit_mb=128', '-max_len=131072',
                    '-artifact_prefix=' + temp + '/']
        else:
            args = [str(binary), *map(str, seeds)]
        try:
            subprocess.run(args, check=True, timeout=60)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            artifacts = a.build.resolve() / 'failures' / name
            artifacts.mkdir(parents=True, exist_ok=True)
            for item in Path(temp).iterdir():
                if item.is_file():
                    shutil.copy2(item, artifacts / item.name)
            raise
