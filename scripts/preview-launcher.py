"""Run the real launcher UI with live files and separate design-preview data."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import webbrowser

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-build', action='store_true', help='Reuse the existing debug executable')
    parser.add_argument('--no-browser', action='store_true', help='Print the URL without opening it')
    parser.add_argument('--target-dir', type=Path, help='Reuse a Cargo build folder on another drive')
    args = parser.parse_args()
    env = os.environ.copy()
    if args.target_dir:
        env['CARGO_TARGET_DIR'] = str(args.target_dir.resolve())
    metadata = json.loads(subprocess.check_output(
        ['cargo', 'metadata', '--no-deps', '--format-version', '1'], cwd=ROOT, env=env, text=True))
    target = Path(metadata['target_directory'])
    target.mkdir(parents=True, exist_ok=True)
    if not args.no_build:
        if shutil.disk_usage(target).free < 4 * 1024**3:
            raise RuntimeError('Build needs at least 4 GiB of free space.')
        subprocess.run(['cargo', 'build', '--locked', '-p', 'mk8-online'], cwd=ROOT, env=env, check=True)
    executable = target / 'debug' / ('Launcher.exe' if os.name == 'nt' else 'Launcher')
    if not executable.is_file():
        raise FileNotFoundError(f'Debug executable not built: {executable}')
    local = ROOT / 'local' / 'design-preview'
    local.mkdir(parents=True, exist_ok=True)
    env['MK8_WEB_ROOT'] = str(ROOT / 'apps' / 'launcher' / 'web')
    # Fixed isolated data folder; never reuse the installed launcher's profile or saves.
    with (local / 'preview.log').open('a', encoding='utf-8') as log:
        process = subprocess.Popen([str(executable), '--headless', '--data-dir', str(local)],
                                   cwd=ROOT, env=env, stdout=log, stderr=log,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            deadline = time.monotonic() + 30
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f'Preview stopped. See {local / "preview.log"}')
                try:
                    state = json.loads((local / 'launcher.json').read_text(encoding='utf-8'))
                    if state.get('pid') == process.pid:
                        break
                except (OSError, ValueError):
                    pass
                if time.monotonic() > deadline:
                    raise RuntimeError(f'Preview did not start. See {local / "preview.log"}')
                time.sleep(.1)
            print(f'Design preview: {state["url"]}', flush=True)
            print(f'Live files: {env["MK8_WEB_ROOT"]}\nSeparate data: {local}\nCtrl+C to stop.', flush=True)
            if not args.no_browser:
                webbrowser.open(state['url'])
            process.wait()
        except KeyboardInterrupt:
            pass
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


if __name__ == '__main__':
    main()
