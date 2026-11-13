"""The Cargo target folder (`cargo metadata`): `<repo>/target` unless `.cargo/config.toml` moves it, as on a PC
whose build folder lives on another drive. Distribution outputs (package, portable, dist, prebuilt-*) go there."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def target_dir():
    metadata = subprocess.check_output(['cargo', 'metadata', '--no-deps', '--format-version', '1'], cwd=ROOT, text=True)
    return Path(json.loads(metadata)['target_directory'])
