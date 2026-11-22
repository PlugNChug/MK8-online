"""Read shader global memory through per-draw copies while the heap import stays for vertex/index.

A capped heap breaks shader global-memory reads above the cap (the HUD digits disappear at 1024 MB); the no-import
gmap path renders them correctly but also copies vertex/index data (multi-second exec hitches at level load). This
mode separates the two: the heap is still imported (and capped) for guest_direct vertex/index reads, while draw
stages take the existing gmap path - bound storage buffers are copied per draw into the transient buffer, the same
path Android uses. BDA is turned off with it: the measurements showed the digits stay missing when the gmap fallback
still went through the heap's device address.

Default on (MK8_GMEM_MAP=0 restores the heap-address shader path). MK8_GMEM_MB=0 + MK8_GMEM_MAP=0 restores the
previous full-import behaviour.
"""
from pathlib import Path

TRANSLATE_OLD = 'bool maxwell_glsl_bda, maxwell_glsl_gmap_gfx;\n'
TRANSLATE_NEW = TRANSLATE_OLD + 'bool maxwell_gmem_no_import;  // MK8 (toolchain/gmem_map.py): the import itself is off\n'

SHADER_OLD = 'extern bool maxwell_glsl_gmap_gfx;\n'
SHADER_NEW = SHADER_OLD + '''// MK8 (toolchain/gmem_map.py): the heap is not imported at all (distinct from the shader gmap mode, which can be
// selected while the heap stays imported for vertex/index).
extern bool maxwell_gmem_no_import;
'''

BDA_OLD = '''    bool no_gmem = !g_vk.host_import || low_vram || getenv("LARECOMP_VK_NO_GMEM");
    g_vk.bda = have12.bufferDeviceAddress && have.features.shaderInt64 && !no_gmem && !getenv("LARECOMP_VK_NO_BDA");
'''
BDA_NEW = '''    bool no_gmem = !g_vk.host_import || low_vram || getenv("LARECOMP_VK_NO_GMEM");
    const char *gmap_env = getenv("MK8_GMEM_MAP");  // MK8 (toolchain/gmem_map.py): default on, =0 uses the heap address
    bool gmap = no_gmem || !gmap_env || gmap_env[0] != '0';
    g_vk.bda = have12.bufferDeviceAddress && have.features.shaderInt64 && !gmap && !getenv("LARECOMP_VK_NO_BDA");
'''

DEVICE_OLD = '''    maxwell_glsl_gmap_gfx = no_gmem;
    LOG("[vk] shader global memory: %s\\n", g_vk.bda ? "the heap through its device address" :
        no_gmem ? "per-draw copies of the storage buffers (no host memory import)" : "the heap as a storage buffer");
'''
DEVICE_NEW = '''    maxwell_gmem_no_import = no_gmem;  // MK8 (toolchain/gmem_map.py)
    maxwell_glsl_gmap_gfx = gmap;
    LOG("[vk] shader global memory: %s\\n", gmap
        ? (no_gmem ? "per-draw copies of the storage buffers (no host memory import)"
                   : "per-draw copies of the storage buffers; the heap import stays for vertex/index")
        : g_vk.bda ? "the heap through its device address" : "the heap as a storage buffer");
'''

INFO_OLD = '''    if (off < 0) off = getenv("LARECOMP_VK_NO_GMEM") != NULL || !g_vk.host_import || maxwell_glsl_gmap_gfx;
'''
INFO_NEW = '''    if (off < 0) off = getenv("LARECOMP_VK_NO_GMEM") != NULL || !g_vk.host_import || maxwell_gmem_no_import;  // MK8 (gmem_map)
'''

LOG_OLD = '''    LOG("[vk] guest heap imported for shader global memory: %llu MB (requested %llu MB) in %.1f ms\\n",
        (unsigned long long)(size >> 20), (unsigned long long)(want >> 20), (SDL_GetTicksNS() - t_imp0) / 1e6);
'''
LOG_NEW = '''    LOG("[vk] guest heap imported%s: %llu MB (requested %llu MB) in %.1f ms\\n",
        maxwell_glsl_gmap_gfx ? " for vertex/index reads" : " for shader global memory",
        (unsigned long long)(size >> 20), (unsigned long long)(want >> 20), (SDL_GetTicksNS() - t_imp0) / 1e6);
'''


def prepare(runtime: Path):
    tr = runtime/'nvn/maxwell/translate.c'
    text = tr.read_text(encoding='utf-8')
    if 'toolchain/gmem_map.py' in text:
        return
    if text.count(TRANSLATE_OLD) != 1:
        raise ValueError('gmem map translate boundary changed')
    tr.write_text(text.replace(TRANSLATE_OLD, TRANSLATE_NEW, 1), encoding='utf-8')
    sh = runtime/'nvn/maxwell/shader.h'
    text = sh.read_text(encoding='utf-8')
    if text.count(SHADER_OLD) != 1:
        raise ValueError('gmem map shader boundary changed')
    sh.write_text(text.replace(SHADER_OLD, SHADER_NEW, 1), encoding='utf-8')
    dev = runtime/'nvn/vulkan/device.c'
    text = dev.read_text(encoding='utf-8')
    for old, new in ((BDA_OLD, BDA_NEW), (DEVICE_OLD, DEVICE_NEW)):
        if text.count(old) != 1:
            raise ValueError('gmem map device boundary changed: ' + old.strip()[:50])
        text = text.replace(old, new, 1)
    dev.write_text(text, encoding='utf-8')
    ex = runtime/'nvn/vulkan/exec.c'
    text = ex.read_text(encoding='utf-8')
    for old, new in ((INFO_OLD, INFO_NEW), (LOG_OLD, LOG_NEW)):
        if text.count(old) != 1:
            raise ValueError('gmem map exec boundary changed: ' + old.strip()[:50])
        text = text.replace(old, new, 1)
    ex.write_text(text, encoding='utf-8')
