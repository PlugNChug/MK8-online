"""Stretch changes presentation only; frame generation must not blend different framing."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import output_aspect as aspect_ratio


HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "nvn/output_scale.h"
#include "nvn/framegen.h"
#ifdef NDEBUG
#error Aspect checks require assertions
#endif
#undef assert
#define assert(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Assertion failed: %s, line %d\n", #condition, __LINE__); \
        exit(1); \
    } \
} while (0)
Settings g_settings;
static double present_mode;
double mk8_ultrawide_present_mode(void) { return present_mode; }
void rt_log(const char *fmt, ...) { (void)fmt; }
HOOK

static void check_scale(void) {
    const int sizes[][2] = {{1920,1080}, {1680,720}, {2560,1080}, {3440,1440}, {3840,1600}, {3840,1080}, {5120,1440}, {1280,960}};
    const double ratios[] = {16.0/9.0,21.0/9.0,2560.0/1080.0,3440.0/1440.0,3840.0/1600.0,32.0/9.0};
    const int modes[] = {RES_MATCH, 1, 2, RES_Q75, RES_Q50, RES_Q25, RES_DYNAMIC};
    assert(!widescreen_allowed());
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        double dw = sizes[i][0], dh = sizes[i][1], fit = fmin(dw / 1920, dh / 1080);
        for (unsigned j = 0; j < sizeof modes / sizeof *modes; j++) {
            OutScale original, stretch, restored;
            g_settings.res_mode = modes[j];
            g_settings.aspect = ASPECT_ORIGINAL;
            output_scale(dw, dh, widescreen_allowed(), 1, -1, &original);
            assert(original.vw == 1920 * fit && original.vh == 1080 * fit);
            assert(original.vx == (dw - original.vw) / 2 && original.vy == (dh - original.vh) / 2);
            g_settings.aspect = ASPECT_UNLOCKED;
            output_scale(dw, dh, widescreen_allowed(), 1, -1, &stretch);
            assert(stretch.vx == 0 && stretch.vy == 0 && stretch.vw == dw && stretch.vh == dh);
            assert(stretch.sx == original.sx && stretch.sy == original.sy);
            assert(stretch.shape == 1 && original.shape == 1);
            g_settings.aspect = ASPECT_ORIGINAL;
            output_scale(dw, dh, widescreen_allowed(), 1, -1, &restored);
            assert(restored.vx == original.vx && restored.vy == original.vy);
            assert(restored.vw == original.vw && restored.vh == original.vh);
            assert(restored.sx == original.sx && restored.sy == original.sy && restored.shape == 1);
            g_settings.aspect = ASPECT_ULTRAWIDE;
            output_scale(dw, dh, widescreen_allowed(), 1, -1, &restored);
            assert(restored.vx==original.vx && restored.vy==original.vy);
            assert(restored.vw==original.vw && restored.vh==original.vh);
            for (unsigned r=0;r<sizeof ratios/sizeof *ratios;r++) {
                present_mode=ratios[r];
                for (int aspect = ASPECT_UNLOCKED; aspect <= ASPECT_ULTRAWIDE; aspect++) {
                    g_settings.aspect = aspect;
                    OutScale queued;
                    output_scale(dw, dh, widescreen_allowed(), 1, -1, &queued);
                    if (present_mode==16.0/9.0) {
                        assert(queued.vx==original.vx && queued.vy==original.vy);
                        assert(queued.vw==original.vw && queued.vh==original.vh);
                    } else {
                        // Fit this image's aspect, independent of the current window's shape and settings.
                        double height=fmin(dh,dw/present_mode), width=height*present_mode;
                        assert(fabs(queued.vw-width)<1e-9 && fabs(queued.vh-height)<1e-9);
                        assert(fabs(queued.vx-(dw-width)/2)<1e-9 && fabs(queued.vy-(dh-height)/2)<1e-9);
                        assert(fabs(queued.vw/queued.vh-present_mode)<1e-12);
                    }
                    assert(queued.sx == original.sx && queued.sy == original.sy && queued.shape == 1);
                }
            }
            present_mode = 0;
        }
    }
}

static void check_override(void) {
    const double tokens[]={0,16.0/9.0,21.0/9.0};
    const double widths[]={1680,1280,1680}, heights[]={472.5,720,720};
    g_settings.aspect=ASPECT_UNLOCKED; g_settings.res_mode=RES_MATCH;
    for (unsigned i=0;i<3;i++) {
        present_mode=tokens[i];
        OutScale o;
        output_scale(1680,720,widescreen_allowed(),1,-1,&o);
        assert(o.vw==widths[i] && o.vh==heights[i]);
        assert(o.vx==(1680-widths[i])/2 && o.vy==(720-heights[i])/2);
        assert(o.shape==1 && o.sx==o.sy);
    }
}

PRESENTERS

static void check_history(bool (*step)(const OutScale *, bool, bool)) {
    /* Same drawable, format and UV crop: only the image's viewport changes. */
    OutScale fitted = {.vx = 440, .vy = 0, .vw = 2560, .vh = 1440};
    OutScale stretched = {.vx = 0, .vy = 0, .vw = 3440, .vh = 1440};
    present_mode=0;
    assert(!step(&fitted, true, false));
    assert(step(&fitted, true, false));
    assert(!step(&stretched, true, false));
    assert(step(&stretched, true, false));
    assert(!step(&fitted, true, false));
    assert(step(&fitted, true, false));
    assert(!step(&fitted, false, false));
    assert(!step(&fitted, true, false));
    assert(step(&fitted, true, false));
    assert(!step(&fitted, true, true));
    assert(!step(&fitted, true, false));
    assert(step(&fitted, true, false));
    // Ordinary Stretch and native wide may share the exact rectangle, format and UV crop.
    assert(!step(&stretched, true, false));
    assert(step(&stretched, true, false));
    const double tokens[]={3440.0/1440.0,32.0/9.0,16.0/9.0,0};
    for (unsigned i=0;i<sizeof tokens/sizeof *tokens;i++) {
        present_mode=tokens[i];
        assert(!step(&stretched,true,false));
        assert(step(&stretched,true,false));
    }
}

PUBLISHER

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "scale")) check_scale();
    else if (!strcmp(argv[1], "publisher")) check_publisher();
    else if (!strcmp(argv[1], "override")) check_override();
    else { check_history(vulkan_step); check_history(metal_step); }
    return 0;
}
'''

PUBLISHER_HARNESS = r'''
struct SDL_Window { int unused; };
typedef struct { unsigned type; } SDL_Event;
enum { SDL_EVENT_QUIT=1, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, SDL_EVENT_WINDOW_MINIMIZED,
       SDL_EVENT_WINDOW_RESTORED, SDL_WINDOW_MINIMIZED=0x40 };
static struct SDL_Window test_window, *window=&test_window;
static _Atomic bool ui_ready;
static _Atomic float g_display_hz;
static unsigned event_step, published, queries, input_events;
static unsigned flags;
static int window_width=1680, window_height=720;
static bool query_ok=true, requests_applied;
#define LOG(...) rt_log(__VA_ARGS__)
void mk8_ultrawide_drawable(int width, int height) {
    const int expected[][2]={{1680,720},{3440,1440},{0,0},{0,0},{5120,1440},{2560,1080}};
    assert(published<6 && width==expected[published][0] && height==expected[published][1]);
    assert(atomic_load(&ui_ready)==(published!=0));
    published++;
}
static unsigned SDL_GetWindowFlags(struct SDL_Window *w) { assert(w==window); return flags; }
static bool SDL_GetWindowSizeInPixels(struct SDL_Window *w, int *width, int *height) {
    assert(w==window); queries++;
    *width=query_ok ? window_width : 999; *height=query_ok ? window_height : 777;
    return query_ok;
}
static void ui_window_init(void) { assert(!published && !atomic_load(&ui_ready)); }
static int rt_reboot_requested(void) { return requests_applied ? 17 : 0; }
static bool SDL_WaitEventTimeout(SDL_Event *event, int timeout) {
    assert(timeout==(event_step==0 ? 16 : 0) && atomic_load(&ui_ready));
    switch (event_step++) {
    case 0: window_width=3440; window_height=1440; event->type=SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED; return true;
    case 1: flags=SDL_WINDOW_MINIMIZED; event->type=SDL_EVENT_WINDOW_MINIMIZED; return true;
    case 2: flags=0; query_ok=false; event->type=SDL_EVENT_WINDOW_RESTORED; return true;
    case 3: query_ok=true; window_width=5120; window_height=1440; event->type=SDL_EVENT_WINDOW_RESTORED; return true;
    default: return false;
    }
}
bool input_user_blocked(const SDL_Event *event) {
    (void)event;
    assert(published==event_step+1);  // publication precedes even a consumed resize event
    return event_step<=2;
}
// This fixture exercises drawable publication with ordinary window events and no controllers.
void mk8_title_pump(void) {}
bool menu_input_captured(void) { return false; }
bool mk8_settings_page_active(void) { return false; }
bool mk8_controller_layout_active(void) { return false; }
bool mk8_title_event(const SDL_Event *event, bool captured) { return false; }
void mk8_tip_event(const SDL_Event *event, bool settings) {}
void mk8_poll_steam_gamepads(void) {}
bool mk8_steam_gamepad_event(const SDL_Event *) { return false; }
void mk8_rumble_pump(void) {}
void mk8_edit_pump(void) {}
void mk8_tip_sync(void) {}
static bool menu_handle_event(const SDL_Event *event) { (void)event; return event_step==3; }
static void input_handle_event(const SDL_Event *event) { (void)event; assert(event_step==4); input_events++; }
static void menu_apply_window_requests(struct SDL_Window *w) {
    assert(w==window && published==5);
    window_width=2560; window_height=1080; requests_applied=true;
}
UI_LOOP
static void check_publisher(void) {
    assert(ui_run()==17);
    assert(published==6 && queries==5 && input_events==1 && atomic_load(&ui_ready));
}
'''


class AspectRatioBoundaryTests(unittest.TestCase):
    def test_unknown_or_duplicate_boundaries_leave_all_files_unchanged(self):
        for broken_index, (broken, _, _) in enumerate(aspect_ratio.EDITS):
            for copies in (0, 2):
                with self.subTest(path=broken, copies=copies), tempfile.TemporaryDirectory() as folder:
                    runtime = Path(folder)
                    sources = {}
                    for index, (name, old, _) in enumerate(aspect_ratio.EDITS):
                        sources[name] = sources.get(name, '') + old * (copies if index == broken_index else 1)
                    for name, content in sources.items():
                        path = runtime/name
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text(content, encoding='utf-8')
                    before = {name: (runtime/name).read_bytes() for name in sources}
                    with self.assertRaisesRegex(ValueError, 'Aspect ratio boundary changed'):
                        aspect_ratio.prepare(runtime)
                    self.assertEqual(before, {name: (runtime/name).read_bytes() for name in before})


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class AspectRatioTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='mk8-aspect-')
        cls.addClassCleanup(temporary.cleanup)
        folder = Path(temporary.name)
        runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', folder/'host')
        hooks = (runtime/'game_hooks.c').read_text(encoding='utf-8')
        start = hooks.index('WEAK bool widescreen_allowed(void)')
        hook = hooks[start:hooks.index('\n', start)].removeprefix('WEAK ')
        presenters = []
        for name, path in [('vulkan', 'nvn/vulkan/device.c'), ('metal', 'nvn/metal.m')]:
            text = (runtime/path).read_text(encoding='utf-8')
            start = text.index('        static FgState fg_state;')
            guard = text[start:text.index('        FgFrame ff =', start)]
            presenters.append('static bool '+name+'_step(const OutScale *o, bool enabled, bool held) {\n'+r'''
    double vx = o->vx, vy = o->vy, vw = o->vw, vh = o->vh;
''' + guard + r'''
    static uint64_t now;
    FgFrame ff = {.w = 3440, .h = 1440, .fmt = 1, .crop = {0, 0, 1, 1},
                  .now_ns = (now += 16666667), .display_hz = 120, .enabled = enabled, .held = held};
    return framegen_step(&fg_state, &ff);
}
''')
        source = folder/'aspect.c'
        device = (runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
        ui_start = device.index('static void mk8_ultrawide_update_drawable(void)')
        ui_loop = device[ui_start:]
        publisher = PUBLISHER_HARNESS.replace('UI_LOOP', ui_loop)
        source.write_text(HARNESS.replace('HOOK', hook).replace('PRESENTERS', '\n'.join(presenters))
                         .replace('PUBLISHER', publisher), encoding='utf-8')
        cls.exe = folder/'aspect.exe'
        result = subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-UNDEBUG',
                                 '-DFG_NO_SDL', '-I'+str(runtime), source, runtime/'nvn/output_scale.c',
                                 runtime/'nvn/framegen.c', '-o', cls.exe], env=compiler_env(), capture_output=True)
        if result.returncode:
            raise AssertionError(result.stderr.decode(errors='replace'))

    def run_case(self, name):
        env = dict(os.environ)
        for key in ('LARECOMP_SCALE', 'LARECOMP_WIDE_TEST'):
            env.pop(key, None)
        if name == 'override':
            env['LARECOMP_WIDE_TEST'] = '2'
        result = subprocess.run([self.exe, name], env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stretch_preserves_render_scale_and_original_framing(self):
        self.run_case('scale')

    def test_image_token_overrides_forced_test_placement(self):
        self.run_case('override')

    def test_frame_generation_restarts_when_presentation_changes(self):
        self.run_case('history')

    def test_ui_publishes_drawable_before_ready_and_consumed_events(self):
        self.run_case('publisher')


if __name__ == '__main__':
    unittest.main()
