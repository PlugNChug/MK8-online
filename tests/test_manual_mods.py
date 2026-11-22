"""Boot the pinned mod preferences and resolve authored assets through both RomFS readers."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER, compiler_env
from host import prepare_host_sources


@unittest.skipUnless(os.name == 'nt' and DEFAULT_COMPILER.is_file(), 'Windows native compiler required')
class InstalledMods(unittest.TestCase):
    def test_installed_romfs_layers(self):
        with tempfile.TemporaryDirectory() as tmp:
            cwd = Path(tmp)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', cwd/'host')
            exe = cwd/'lookup.exe'
            subprocess.run([str(DEFAULT_COMPILER),'cc','-O1','-UNDEBUG','-target','x86_64-windows-gnu',
                            '-I',str(runtime),str(ROOT/'tests/fixtures/manual_mods.c'),str(runtime/'usermods.c'),
                            str(runtime/'mods.c'),str(runtime/'romfs_build.c'),
                            str(ROOT/'runtime/mk8/support/romfs_overlay.c'),
                            str(ROOT/'runtime/mk8/support/settings_menu.c'),'-o',str(exe)],
                           env=compiler_env(),check=True)
            def asset(folder, path, content):
                file = cwd/folder/path
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(content)

            path = 'Course/sample.bin'
            for folder, content in [('base','base'),('mods/A/romfs','first'),('mods/B/romfs','second'),
                                    ('explicit/romfs','explicit'),('pack','pack')]:
                asset(folder,path,content)
            def run(*args, **env):
                clean = dict(os.environ, RECOMP_MOD_DIR='', MK8_ROMFS_OVERLAY='', MK8_NEXTENDO_TEST='0',
                             MK8_SETTINGS_EXPERIMENT='0', MK8_SETTINGS_ROMFS='')
                result = subprocess.run([str(exe),*args], cwd=cwd, env={**clean,**env}, capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            run('enable')  # Discover installed folders off by default, then toggle through the real Mods service.
            run('read',path,'first')
            run('disable')
            run('read',path,'base')
            run('soft-reset')
            (cwd/'mods/mods.ini').write_text('A=1\nB=1\n')
            run('read',path,'second',MK8_ROMFS_OVERLAY='pack')
            asset('mods/B/romfs','NewFolder/Sample.bin','added')
            run('read','NewFolder/Sample.bin','added')
            run('read',path,'explicit',RECOMP_MOD_DIR='explicit')
            run('read',path,'base',MK8_NEXTENDO_TEST='1')
            common, menu = 'UI/cmn/common.sarc', 'UI/cmn/menu.sarc'
            asset('settings-menu/romfs',menu,'settings')
            asset('settings-menu/romfs',common,'settings-roster')
            asset('controller-menu/romfs',common,'roster')
            native = dict(MK8_SETTINGS_EXPERIMENT='1')
            run('read',common,'settings-roster','1','1',**native)
            asset('mods/B/romfs',menu,'custom-menu')
            run('read',menu,'custom-menu','0','1',**native)
            run('read',common,'roster','0','1',**native)
            asset('mods/A/romfs',common,'custom-common')
            run('read',common,'custom-common','0','0',**native)
            run('read',menu,'custom-menu','0','0',**native)
            run('read',common,'custom-common','0','0')
            (cwd/'mods/mods.ini').write_text('A=0\nB=0\n')
            run('read',menu,'settings','1','1',**native)
            # The last entry still fits and changes restart state, even at the scanner's ID/count limits.
            ids = [f'{i:03}' + 'p'*92 for i in range(128)]
            for mod in ids:
                asset(f'mods/{mod}/romfs',path,mod)
            (cwd/'mods/mods.ini').write_text('../explicit=1\n' + ''.join(f'{i}=1\n' for i in ids))
            run('full-list',ids[-1])
            # The real outer mods.c buffer must reject a truncated explicit root.
            oversized = subprocess.run([str(exe),'read',path,'base'],cwd=cwd,
                env=dict(os.environ,RECOMP_MOD_DIR='x'*18000,MK8_SETTINGS_EXPERIMENT='0',
                         MK8_ROMFS_OVERLAY='',MK8_NEXTENDO_TEST='0'),capture_output=True,text=True)
            self.assertNotEqual(oversized.returncode,0)
            self.assertIn('overlay paths too long',oversized.stderr)
