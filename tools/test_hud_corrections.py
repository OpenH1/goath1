"""Supplied reticle/zoom artwork fidelity and CE sprite selection."""
import hashlib
import json
from pathlib import Path

import pytest

np = pytest.importorskip("numpy")
Image = pytest.importorskip("PIL.Image")

from harness import function
from hud_assets import build_asset
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'port/assets/hud'


def run_c(tmp_path, code):
    compiler = shutil.which('clang')
    if not compiler:
        pytest.skip('clang required')
    source = tmp_path / 'style.c'
    binary = tmp_path / 'style.exe'
    source.write_text(code)
    target = ['--target=i686-pc-windows-msvc'] if sys.platform == 'win32' else ['-m32']
    result = subprocess.run([compiler, *target, '-std=gnu89', '-O1', str(source), '-o', str(binary)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    subprocess.run([str(binary)], check=True, timeout=10)


def test_supplied_sources_are_unchanged():
    manifest = json.loads((ASSETS / 'corrections/source-sha256.json').read_text())
    for name, record in manifest.items():
        assert hashlib.sha256((ASSETS / 'corrections' / name).read_bytes()).hexdigest() == record['sha256']


def test_packed_corrections_use_supplied_pixels():
    for filename in ['layout.json', 'ce_layout.json']:
        for entry in json.loads((ASSETS / filename).read_text())['assets']:
            if not any('source_png' in cell for cell in entry['cells']):
                continue
            actual = np.asarray(Image.open(ASSETS / (entry['name']+'.png')))
            assert np.array_equal(actual, build_asset(entry, {}))
            # These assets are monochrome premultiplied coverage, not opaque
            # white pictures with black backgrounds or black letter holes.
            assert np.array_equal(actual[..., 0], actual[..., 3])
            assert np.array_equal(actual[..., 1], actual[..., 3])
            assert np.array_equal(actual[..., 2], actual[..., 3])
            for cell in entry['cells']:
                with Image.open(ASSETS / cell['source_png']) as supplied:
                    source = supplied.convert('L').crop(cell['crop'])
                x0,y0,x1,y1 = [round(v*entry['scale']) for v in cell['xbox']]
                ratio = min((x1-x0)/source.width, (y1-y0)/source.height)
                size = tuple(round(n*ratio) for n in source.size)
                x0 += (x1-x0-size[0])//2
                y0 += (y1-y0-size[1])//2
                expected = np.asarray(source.resize(size, Image.Resampling.LANCZOS))
                assert np.array_equal(actual[y0:y0+size[1], x0:x0+size[0], 3], expected)
