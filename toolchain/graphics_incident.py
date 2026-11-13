"""Bounded graphics evidence; patch only known prepared host boundaries."""
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]

SKIP_HELPER = '''
// MK8: diagnostics observe the actual draw rejection, not an intermediate wait.
static __thread bool mk8_gfx_budget_miss;
static __thread double mk8_gfx_remaining;
'''

REPORT_HELPER = '''
static void mk8_gfx_program_skip(Prog *p, uint64_t pipeline, int state) {
    if (!mk8_gfx_enabled()) return;
    mk8_gfx_skip(vk_frame_counter() + 1, p ? p->code : NULL, pipeline, state,
                 shader_mode(), mk8_targets_redrawn(), mk8_gfx_budget_miss, mk8_gfx_remaining);
}
'''

DEVICE_BEGIN = '''        // Consume only on the render owner, after existing screenshot arbitration.
        bool incident = mk8_gfx_begin(g_frame_counter + 1, shot || req,
            g_vk.sc_extent.width, g_vk.sc_extent.height, g_vk.sc_format,
            g_vk.props.driverVersion, g_vk.props.vendorID, g_vk.props.deviceID, fgc != NULL,
            shot_path, sizeof shot_path);
'''


def edits():
    return {
        'nvn/vulkan/exec.c': [
            ('#define SKIP_DRAW() do { stat_skipped++;',
             '#define SKIP_DRAW() do { mk8_gfx_preview_skip(__LINE__); stat_skipped++;',1),
            ('static void exec_list(uint64_t handle, int depth) {',
             (ROOT/'runtime/mk8/graphics/preview_draw.inc').read_text(encoding='utf-8')+'\n'
             'static void exec_list(uint64_t handle, int depth) {',1),
            ('        case CMD_DRAW_ARRAYS: if (!st.render_off) draw(r, false); break;',
             '''        case CMD_DRAW_ARRAYS:
            if (mk8_gfx_preview_active()) mk8_preview_command(r, false);
            if (!st.render_off) draw(r, false);
            mk8_gfx_preview_draw_end();
            break;''',1),
            ('        case CMD_DRAW_ELEMENTS: if (!st.render_off) draw(r, true); break;',
             '''        case CMD_DRAW_ELEMENTS:
            if (mk8_gfx_preview_active()) mk8_preview_command(r, true);
            if (!st.render_off) draw(r, true);
            mk8_gfx_preview_draw_end();
            break;''',1),
            ('        if (!use_index) vkCmdDraw(cb, count, run_n[ri], first, base_inst + run_at[ri]);',
             '        mk8_gfx_preview_emit(pass_w, pass_h);\n'
             '        if (!use_index) vkCmdDraw(cb, count, run_n[ri], first, base_inst + run_at[ri]);',1),
            ('                bool eq = q ? q[0] == q[2] : true;',
             '''                // Read exactly the original two scalar operands, once; trace the values actually compared.
                uint64_t mk8_a = q ? q[0] : 0, mk8_b = q ? q[2] : 0;
                bool eq = q ? mk8_a == mk8_b : true;''',1),
            ('                st.render_off = r->x[0] == 0 ? !eq : eq;',
             '                st.render_off = r->x[0] == 0 ? !eq : eq;\n'
             '''                if (mk8_gfx_preview_active()) {
                    uint64_t id = mk8_gfx_counter_compare(vk_frame_counter()+1, vk_pending_seq(), r->x[0], r->x[1],
                        q != NULL, mk8_a, mk8_b, eq, st.render_off);
                    mk8_gfx_preview_condition(r->x[0], true, eq, mk8_gfx_token(r->x[1]), id);
                }''',1),
            ('                st.render_off = !r->x[0];',
             '                st.render_off = !r->x[0];\n'
             '                if (mk8_gfx_preview_active()) mk8_gfx_preview_condition(r->x[0], false, false, 0, 0);',1),
            ('static void cq_resolve_done(uint64_t done) {  // every slot whose submission is done\n',
             'static void cq_resolve_done(uint64_t done) {  // every slot whose submission is done\n'
             '    mk8_gfx_counter_complete(done); // reuse the completion already observed; no new query or wait\n',1),
            ('        VK_CHECK(sr);',
             '        VK_CHECK(sr);\n        mk8_gfx_counter_submit(vk_frame_counter()+1, submit_seq+1);',1),
            ('                        o->rep_after[o->nrep++] = o->n;',
             '''                        o->rep_after[o->nrep++] = o->n;
                        mk8_gfx_counter_queue(vk_frame_counter()+1, o->seq, reset_samples ? 0 : (uint64_t)(uintptr_t)out,
                            o->nrep-1, o->n, o->n, true, reset_samples);''',1),
            ('                    }\n                    pthread_mutex_unlock(&cq_lock);\n                }\n                break;  // (upstream\'s CPU-time write below is for the other counters)',
             '''                    } else {
                        mk8_gfx_counter_queue(vk_frame_counter()+1, o->seq, reset_samples ? 0 : (uint64_t)(uintptr_t)out,
                            o->nrep, o->n, o->n, false, reset_samples);
                    }
                    pthread_mutex_unlock(&cq_lock);
                }
                break;  // (upstream's CPU-time write below is for the other counters)''',1),
            ('        if (o->rep_out[k]) { o->rep_out[k][0] = base + sum; o->rep_out[k][1] = ns_to_gpu_ticks(plat_now_ns()); }',
             '''        if (o->rep_out[k]) {
            o->rep_out[k][0] = base + sum; o->rep_out[k][1] = ns_to_gpu_ticks(plat_now_ns());
            mk8_gfx_counter_write(o->seq, (uint64_t)(uintptr_t)o->rep_out[k], base + sum, base, sum,
                k, o->rep_after[k], o->n, ok);
            mk8_gfx_occlusion_result(o->seq, (uint64_t)(uintptr_t)o->rep_out[k], base + sum, ok);
        }''',1),
            ('        else { base = 0; sum = 0; }  // ResetCounter at this point in GPU order.',
             '''        else {
            uint64_t old_base = base, old_sum = sum;
            base = 0; sum = 0;
            mk8_gfx_counter_reset(o->seq, k, old_base, old_sum);
        }  // ResetCounter at this point in GPU order.''',1),
            ('            if (out) { out[0] = (uint32_t)r->x[0]; out[1] = ns_to_gpu_ticks(plat_now_ns()); }',
             '''            if (out) {
                out[0] = (uint32_t)r->x[0]; out[1] = ns_to_gpu_ticks(plat_now_ns());
                mk8_gfx_counter_value(vk_frame_counter()+1, vk_pending_seq(), (uint64_t)(uintptr_t)out, (uint32_t)r->x[0]);
            }''',1),
            ('static double mk8_shader_budget_ms(void) {', SKIP_HELPER+'static double mk8_shader_budget_ms(void) {', 1),
            ('    spent += (SDL_GetTicksNS() - t0) / 1e6;',
             '    if (mk8_gfx_enabled()) { mk8_gfx_budget_miss |= !ok; mk8_gfx_remaining = budget - spent; }\n'
             '    spent += (SDL_GetTicksNS() - t0) / 1e6;', 1),
            ('    return atomic_load(&p->state) == BUILD_READY;\n}',
             '    return atomic_load(&p->state) == BUILD_READY;\n}'+REPORT_HELPER, 1),
            ('if (!prog_ready(pr)) SKIP_DRAW();',
             'mk8_gfx_budget_miss = false; mk8_gfx_remaining = 0;\n'
             '    if (!prog_ready(pr)) { mk8_gfx_program_skip(pr, 0, pr ? atomic_load(&pr->state) : -1); SKIP_DRAW(); }', 2),
            ('    if (shader_mode() == SHADERS_PREFETCH && atomic_load(&pp->state) == BUILD_PENDING) {',
             '    mk8_gfx_budget_miss = false; mk8_gfx_remaining = 0;\n'
             '    if (shader_mode() == SHADERS_PREFETCH && atomic_load(&pp->state) == BUILD_PENDING) {', 1),
            ('    if (atomic_load(&pp->state) != BUILD_READY) SKIP_DRAW();',
             '''    if (atomic_load(&pp->state) != BUILD_READY) {
        if (mk8_gfx_enabled()) {
            PipeKey diagnostic_key = k;
            diagnostic_key.prog = NULL; // remove ASLR-dependent host pointer from the evidence
            uint64_t id = hash_words(0xcbf29ce484222325ull, &diagnostic_key, sizeof diagnostic_key);
            id = hash_words(id, pr->code, sizeof pr->code);
            mk8_gfx_program_skip(pr, id, atomic_load(&pp->state));
        }
        SKIP_DRAW();
    }''', 1),
        ],
        'hle/input.c': [
            ('void input_handle_event(const SDL_Event *e) {',
             '''void input_handle_event(const SDL_Event *e) {
    if ((e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) &&
        e->key.scancode == SDL_SCANCODE_F8 && mk8_gfx_enabled()) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) mk8_gfx_request();
        return;
    }
    if ((e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) &&
        e->key.scancode == SDL_SCANCODE_F7 && mk8_gfx_enabled()) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) mk8_gfx_preview_request();
        return;
    }''', 1),
        ],
        'nvn/vulkan/device.c': [
            ('        if (fg_gen) vk_frame_end_at(shot || req ? shot_path : NULL, t_gen + FG_HALF_SLOT_NS);\n'
             '        else vk_frame_end(shot || req ? shot_path : NULL);',
             DEVICE_BEGIN+'        if (fg_gen) vk_frame_end_at(shot || req || incident ? shot_path : NULL, t_gen + FG_HALF_SLOT_NS);\n'
             '        else vk_frame_end(shot || req || incident ? shot_path : NULL);\n'
             '        if (incident) mk8_gfx_finish(vk_save_png);', 1),
            ('void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {',
             'void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {\n'
             '    if (mk8_gfx_preview_image(path, rgba, w, h)) return;',1),
            ('    if (!s || !SDL_SavePNG(s, path)) LOG(',
             '    bool mk8_saved = s && SDL_SavePNG(s, path);\n'
             '    mk8_gfx_png_result(path, mk8_saved);\n'
             '    if (!mk8_saved) LOG(', 1),
            ('    if (!save_png_plain(path, rgba, w, h)) LOG(',
             '    bool mk8_saved = save_png_plain(path, rgba, w, h);\n'
             '    mk8_gfx_png_result(path, mk8_saved);\n'
             '    if (!mk8_saved) LOG(', 1),
            ('        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));\n    }\n}',
             '        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));\n    }\n'
             '    mk8_gfx_swapchain(vk_frame_counter() + 1, ext.width, ext.height, sf.format);\n}', 1),
        ],
        'nvn/vulkan/textures.c': [
            ('void renderer_set_scale(float sx, float sy) { g_rs_x = sx; g_rs_y = sy; }',
             '''void renderer_set_scale(float sx, float sy) {
    if (mk8_gfx_enabled()) {
        static bool seen;
        if (!seen || sx != g_rs_x || sy != g_rs_y) {
            mk8_gfx_scale(vk_frame_counter() + 1, sx, sy);
            seen = true;
        }
    }
    g_rs_x = sx; g_rs_y = sy;
}''', 1),
        ],
        'ui/menu.mm': [
            ('            g_settings.fullscreen = !g_settings.fullscreen;',
             '            mk8_gfx_fullscreen_request(e->key.scancode == SDL_SCANCODE_F11 ? 1 : 2, !g_settings.fullscreen);\n'
             '            g_settings.fullscreen = !g_settings.fullscreen;', 1),
            ('        g_settings.fullscreen = fs;',
             '        mk8_gfx_fullscreen_request(3, fs);\n        g_settings.fullscreen = fs;', 1),
            ('        SDL_SetWindowFullscreen(w, g_settings.fullscreen);',
             '''        bool mk8_fullscreen_ok = SDL_SetWindowFullscreen(w, g_settings.fullscreen);
        if (mk8_gfx_enabled()) {
            int mk8_w = 0, mk8_h = 0;
            SDL_GetWindowSizeInPixels(w, &mk8_w, &mk8_h);
            mk8_gfx_fullscreen_apply(g_settings.fullscreen, mk8_fullscreen_ok,
                (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0,
                mk8_w > 0 ? (unsigned)mk8_w : 0, mk8_h > 0 ? (unsigned)mk8_h : 0);
        }''', 1),
        ],
    }


def build_metadata():
    def git(*args):
        try:
            result = subprocess.run(['git', '-C', str(ROOT), *args], capture_output=True, text=True)
        except OSError:
            return None
        return result.stdout.strip() if not result.returncode else None
    commit = git('rev-parse', 'HEAD')
    branch = git('branch', '--show-current')
    status = git('status', '--porcelain')
    if not re.fullmatch(r'[0-9a-f]{40}', commit or ''):
        commit = 'unknown'
    if not re.fullmatch(r'[A-Za-z0-9_./-]{1,120}', branch or ''):
        branch = 'unknown'
    return ('#define MK8_INCIDENT_COMMIT '+json.dumps(commit)+'\n'
            '#define MK8_INCIDENT_BRANCH '+json.dumps(branch)+'\n'
            '#define MK8_INCIDENT_DIRTY '+json.dumps('null' if status is None else 'true' if status else 'false')+'\n')


def prepare(runtime):
    prepared = {}
    # Validate every boundary before writing any instrumentation.
    for relative, changes in edits().items():
        text = (runtime/relative).read_text(encoding='utf-8')
        for old, new, count in changes:
            if text.count(old) != count:
                raise ValueError(f'Graphics incident boundary changed: {relative}: {old[:70]}')
            text = text.replace(old, new)
        prefix='#include "mk8_graphics_incident.h"\n'
        if relative in ('nvn/vulkan/device.c','nvn/vulkan/textures.c'):
            prefix+='extern uint64_t vk_frame_counter(void);\n'
        prepared[runtime/relative] = prefix+text
    for path, text in prepared.items():
        path.write_text(text, encoding='utf-8')
    (runtime/'mk8_graphics_incident.h').write_text(
        (ROOT/'runtime/mk8/graphics/incident.h').read_text(encoding='utf-8'), encoding='utf-8')
    (runtime/'preview.h').write_text((ROOT/'runtime/mk8/graphics/preview.h').read_text(encoding='utf-8'),encoding='utf-8')
    (runtime/'mk8_incident_build.h').write_text(build_metadata(), encoding='utf-8')
