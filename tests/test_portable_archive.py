"""A relocated thin archive must still link its real native object files."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from native import DEFAULT_COMPILER, compiler_env, write_archive


class PortableArchiveTests(unittest.TestCase):
    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_archive_links_after_its_original_directory_disappears(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            old = base / 'before'; old.mkdir()
            (old / 'objects').mkdir(); (old / 'build').mkdir()
            source = old / 'objects/code.c'
            source.write_text('int answer(void) { return 42; }')
            def run(args):
                subprocess.run([DEFAULT_COMPILER, *args], check=True, capture_output=True, env=compiler_env())
            obj = source.with_suffix('.o')
            run(['cc', '-target', 'x86_64-windows-gnu', '-c', source, '-o', obj])
            write_archive(DEFAULT_COMPILER, old / 'build/code.a', [obj])
            moved = base / 'after'
            old.rename(moved)
            main = moved / 'main.c'
            main.write_text('int answer(void); int main(void) { return answer() == 42 ? 0 : 1; }')
            exe = moved / 'test.exe'
            run(['cc', '-target', 'x86_64-windows-gnu', main, moved / 'build/code.a', '-o', exe])
            result = subprocess.run([exe], timeout=20)
            self.assertEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
