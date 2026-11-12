"""Recorte validado e metadados do readback preservados na recriação da swapchain."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import capture_resize


HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#define LOG(...) ((void)0)
#define FATAL(...) abort()
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
static struct { VkQueue queue; VkSwapchainKHR swapchain; VkExtent2D sc_extent;
                VkFormat sc_format; VkImage sc_images[8]; } g_vk;
static struct { bool ok; void *off; unsigned image; VkSemaphore acquired; } cur_frame;
static VkSemaphore done_sem[8];
static uint64_t last_frame_seq;
typedef int pthread_t;
static int dp_lock, dp_cond;
static struct { unsigned img; VkSemaphore sem; VkSwapchainKHR sc;
                uint64_t at, gen_seq; bool pending; } dp;
static int pthread_create(pthread_t *t, void *a, void *(*f)(void *), void *b) { abort(); }
static void pthread_detach(pthread_t t) { abort(); }
static void pthread_mutex_lock(int *p) { abort(); }
static void pthread_mutex_unlock(int *p) { abort(); }
static void pthread_cond_broadcast(int *p) { abort(); }
static void *dp_main(void *p) { abort(); }
static VkCommandBuffer vk_cmd(void) { return VK_NULL_HANDLE; }
static void vk_present_flush(void) {}
static void vkCmdEndRendering(VkCommandBuffer cb) {}
static void image_barrier(VkCommandBuffer cb, VkImage i, VkImageLayout a, VkImageLayout b) {}
static VkSemaphore binary_semaphore(void) { return (VkSemaphore)1; }
static uint64_t vk_pending_seq(void) { return 1; }
static void vk_submit_extra(VkSemaphore a, VkSemaphore b, void *c) {}
static void vk_queue_lock(void) {}
static void vk_queue_unlock(void) {}
static void vk_wait_seq(uint64_t seq) {}
static VkResult result;
static VkExtent2D original, replacement;
static VkFormat original_format, replacement_format;
static int saved, recreated;
static uint8_t *allocation, *pixels;
static VkResult vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *p) { return result; }
static void recreate_swapchain(void) {
    recreated++;
    g_vk.sc_extent = replacement;
    g_vk.sc_format = replacement_format;
}
static bool recreate_surface(void) { recreate_swapchain(); return true; }
static VkRect2D expected_rect;
static void save_window_image(VkCommandBuffer cb, VkRect2D rect, VkSpan *out) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    allocation = VirtualAlloc(NULL, 2*info.dwPageSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    assert(allocation);
    DWORD old;
    assert(VirtualProtect(allocation+info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old));
    expected_rect = rect;
    size_t bytes = (size_t)rect.extent.width*rect.extent.height*4;
    assert(bytes < info.dwPageSize);
    pixels = allocation+info.dwPageSize-bytes;
    for (size_t i=0; i<bytes; i+=4) {
        pixels[i]=11; pixels[i+1]=22; pixels[i+2]=33; pixels[i+3]=0;
    }
    out->ptr = pixels;
}
static void vk_save_png(const char *path, const void *data, uint32_t w, uint32_t h) {
    assert(w==expected_rect.extent.width && h==expected_rect.extent.height);
    assert(data==pixels);
    bool bgr = original_format==VK_FORMAT_B8G8R8A8_UNORM || original_format==VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i=0; i<(size_t)w*h*4; i+=4) {
        assert(pixels[i]==(bgr ? 33 : 11));
        assert(pixels[i+1]==22 && pixels[i+2]==(bgr ? 11 : 33) && pixels[i+3]==255);
    }
    saved++;
}
'''

MAIN = r'''
int main(void) {
    VkResult results[] = {VK_ERROR_OUT_OF_DATE_KHR, VK_SUBOPTIMAL_KHR, VK_ERROR_SURFACE_LOST_KHR, VK_SUCCESS};
    VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
    for (unsigned r=0; r<4; r++) for (unsigned f=0; f<3; f++) for (unsigned grow=0; grow<2; grow++) for (unsigned crop=0; crop<2; crop++) {
        _putenv_s("MK8_CAPTURE_REGION",crop ? "1,1,1,1" : "");
        original = grow ? (VkExtent2D){4,4} : (VkExtent2D){2,2};
        replacement = grow ? (VkExtent2D){2,2} : (VkExtent2D){4,4};
        original_format = formats[f];
        replacement_format = formats[(f+1)%3];
        g_vk.sc_extent=original; g_vk.sc_format=original_format;
        result=results[r]; saved=0; recreated=0; cur_frame.ok=true;
        frame_end("capture.png", 0);
        assert(saved==1 && recreated==(result!=VK_SUCCESS) && !cur_frame.ok);
        assert(VirtualFree(allocation, 0, MEM_RELEASE));
    }
    cur_frame.ok=false;
    frame_end("capture.png", 0);
    assert(saved==1);
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class CaptureResizeTests(unittest.TestCase):
    def test_capture_region_bounds_and_gpu_copy(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', output)
            text = (runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            start = text.index('static VkRect2D mk8_capture_rect(')
            end = text.index('// End rendering into the window image', start)
            source = output/'region.c'
            source.write_text(r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define FATAL(...) exit(23)
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
static struct { VkExtent2D sc_extent; VkImage sc_images[1]; } g_vk;
static struct { unsigned image; } cur_frame;
static uint64_t bytes;
static VkBufferImageCopy copied;
static VkSpan vk_readback(uint64_t n) { bytes=n;return (VkSpan){0}; }
static void image_barrier(VkCommandBuffer cb,VkImage image,VkImageLayout before,VkImageLayout after) {}
static void vkCmdCopyImageToBuffer(VkCommandBuffer cb,VkImage image,VkImageLayout layout,VkBuffer buf,uint32_t count,const VkBufferImageCopy *copy) {
 assert(count==1);copied=*copy;
}
''' + text[start:end] + r'''
int main(int argc,char **argv) {
 assert(argc==2);_putenv_s("MK8_CAPTURE_REGION",argv[1]);
 g_vk.sc_extent=(VkExtent2D){1280,720};
 VkRect2D rect=mk8_capture_rect(g_vk.sc_extent);VkSpan span;
 save_window_image(VK_NULL_HANDLE,rect,&span);
 assert(bytes==(uint64_t)rect.extent.width*rect.extent.height*4);
 assert(copied.imageOffset.x==rect.offset.x && copied.imageOffset.y==rect.offset.y);
 assert(copied.imageExtent.width==rect.extent.width && copied.imageExtent.height==rect.extent.height);
 assert(copied.imageExtent.depth==1 && copied.imageOffset.z==0);
 if(!*argv[1]) assert(!rect.offset.x && !rect.offset.y && rect.extent.width==1280 && rect.extent.height==720);
 else { unsigned x,y,w,h;assert(sscanf(argv[1],"%u,%u,%u,%u",&x,&y,&w,&h)==4);
  assert(rect.offset.x==x && rect.offset.y==y && rect.extent.width==w && rect.extent.height==h); }
 return 0;
}
''', encoding='utf-8')
            exe = output/'region.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                                     '-O1', '-UNDEBUG', '-I'+str(DEPS/'include'), str(source),
                                     '-o', str(exe)], capture_output=True, text=True, env=compiler_env())
            self.assertEqual(result.returncode, 0, result.stderr[-3000:])
            for value in ('', '0,0,1280,720', '1000,20,280,160', '1279,719,1,1'):
                result = subprocess.run([str(exe), value], timeout=20)
                self.assertEqual(result.returncode, 0, value)
            for value in ('1,2,3', '0,0,0,4', '0,0,1,0', '1279,0,2,1', '0,719,1,2',
                          '-1,0,1,1', '0,0,1,1x', '0,0,1,1,2', '4294967296,0,1,1',
                          '99999999999999999999999999,0,1,1'):
                result = subprocess.run([str(exe), value], timeout=20)
                self.assertEqual(result.returncode, 23, value)

    def test_present_resize_keeps_readback_extent_and_format(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', output)
            text = (runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            function = text[text.index('static void frame_end('):text.index('\nvoid vk_frame_end(')]
            source = output/'capture.c'
            source.write_text(HARNESS+capture_resize.CAPTURE_RECT+function+MAIN, encoding='utf-8')
            exe = output/'capture.exe'
            result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu',
                                     '-O1', '-UNDEBUG', '-I'+str(DEPS/'include'), str(source),
                                     '-o', str(exe)], capture_output=True, text=True, env=compiler_env())
            self.assertEqual(result.returncode, 0, result.stderr[-3000:])
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_unknown_or_duplicate_boundary_is_rejected_without_writing(self):
        original = (ROOT/'upstream/smm2recomp/recomp/runtime/nvn/vulkan/device.c').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory() as folder:
            runtime = Path(folder)
            device = runtime/'nvn/vulkan/device.c'
            device.parent.mkdir(parents=True)
            for old, _ in capture_resize.BOUNDARIES:
                for replacement in ('changed', old+'\n'+old):
                    text = original.replace(old, replacement)
                    device.write_text(text, encoding='utf-8')
                    with self.assertRaisesRegex(ValueError, 'boundary changed'):
                        capture_resize.prepare(runtime)
                    self.assertEqual(device.read_text(encoding='utf-8'), text)


if __name__ == '__main__':
    unittest.main()
