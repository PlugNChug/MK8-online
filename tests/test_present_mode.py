"""Without MAILBOX the swapchain uses FIFO_RELAXED, so a late frame is not held for the next vblank."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import env_cache  # (reads are cached after every other patch)
import present_mode

FIXTURE = r'''
#include <stdint.h>
#include <string.h>
typedef enum { VK_PRESENT_MODE_IMMEDIATE_KHR = 0, VK_PRESENT_MODE_MAILBOX_KHR = 1, VK_PRESENT_MODE_FIFO_KHR = 2,
               VK_PRESENT_MODE_FIFO_RELAXED_KHR = 3 } VkPresentModeKHR;
''' + '%s' + r'''
#define PICK(env, ...) ({ VkPresentModeKHR m[] = {__VA_ARGS__}; mk8_pick_present_mode(m, sizeof m / sizeof *m, env); })
int main(void) {
    /* AMD on Windows (RX 580, driver 26.1.1): no MAILBOX */
    if (PICK(NULL, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR)
        != VK_PRESENT_MODE_FIFO_RELAXED_KHR) return 1;
    /* MAILBOX offered: unchanged */
    if (PICK(NULL, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_MAILBOX_KHR)
        != VK_PRESENT_MODE_MAILBOX_KHR) return 2;
    /* only FIFO (always offered) */
    if (PICK(NULL, VK_PRESENT_MODE_FIFO_KHR) != VK_PRESENT_MODE_FIFO_KHR) return 3;
    if (PICK("", VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR) != VK_PRESENT_MODE_FIFO_RELAXED_KHR) return 4;
    /* an explicit choice wins, and falls back to FIFO when missing */
    if (PICK("fifo", VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR) != VK_PRESENT_MODE_FIFO_KHR) return 5;
    if (PICK("immediate", VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR) != VK_PRESENT_MODE_IMMEDIATE_KHR) return 6;
    if (PICK("mailbox", VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR) != VK_PRESENT_MODE_FIFO_KHR) return 7;
    if (PICK("relaxed", VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR) != VK_PRESENT_MODE_FIFO_KHR) return 8;
    if (PICK("unknown", VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR) != VK_PRESENT_MODE_FIFO_KHR) return 9;
    return 0;
}
'''


class PresentModeTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            device = Path(folder)/'nvn/vulkan/device.c'
            device.parent.mkdir(parents=True)
            device.write_text('static VkPresentModeKHR present_mode(void) {\n}\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                present_mode.prepare(Path(folder))

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_late_frames_are_not_held_without_mailbox(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            text = env_cache.plain((runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8'))
            self.assertIn('mk8_pick_present_mode(m, n, getenv("LARECOMP_VK_PRESENT"))', text)
            self.assertIn('[vk] present mode %s', text)
            start = text.index('static VkPresentModeKHR mk8_pick_present_mode(')
            helper = text[start:text.index('\n}\n', start) + 3]
            source = out/'present_mode.c'
            source.write_text(FIXTURE % helper, encoding='utf-8')
            exe = out/'present_mode.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', str(source), '-o', str(exe)],
                           env=compiler_env(), check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
