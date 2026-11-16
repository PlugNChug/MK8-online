"""Antecipação experimental de compute usando a fila do tradutor, sem compile-ahead global."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Compute preparation boundary changed: {old[:90]}')
    return text.replace(old, new)


def prepare(runtime):
    source = ROOT/'runtime/mk8/graphics'
    shutil.copyfile(source/'compute_prepare.h', runtime/'nvn/mk8_compute_prepare.h')
    objects = runtime/'nvn/objects.c'
    text = objects.read_text(encoding='utf-8')
    boundary = '// MK8 (toolchain/async_submit.py): submissions, presents and queue fences translated in order on one thread\n'
    text = replace(text, boundary, boundary + '#include "buildsync.h"\n#include "mk8_compute_prepare.h"\n')
    text = replace(text, 'enum { MK8_Q_SUBMIT,', 'enum { MK8_Q_COMPUTE_PREPARE, MK8_Q_SUBMIT,')
    text = replace(text, '    int kind, n;\n', '    int kind, n;\n    Mk8ComputeSnapshot *compute;\n')
    text = replace(text, 'static void *mk8_q_worker(void *arg) {',
                   'static _Atomic unsigned mk8_compute_queued;\nstatic void *mk8_q_worker(void *arg) {')
    text = replace(text, '        } else if (it->kind == MK8_Q_PRESENT) {', '''        } else if (it->kind == MK8_Q_COMPUTE_PREPARE) {
            mk8_compute_prepare(it->compute);
            atomic_fetch_sub(&mk8_compute_queued, 1);
        } else if (it->kind == MK8_Q_PRESENT) {''')
    queue = (source/'compute_prepare_queue.inc').read_text(encoding='utf-8')
    queue = queue.replace('static _Atomic unsigned mk8_compute_queued;\n', '')
    text = replace(text, 'static void mk8_q_wait(uint64_t ticket) {', queue + '\nstatic void mk8_q_wait(uint64_t ticket) {')
    boundary = '    if ((p->stage_mask & 3) == 3) {  // (compile-ahead:'
    text = replace(text, boundary, '    if (p->stage_mask & (1u << 5)) mk8_compute_set(&p->stages[5]);\n' + boundary)
    objects.write_text(text, encoding='utf-8')

    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    text = replace(text, '#include "../buildsync.h"', '#include "../buildsync.h"\n#include "../mk8_compute_prepare.h"')
    text = replace(text, 'static int g_cs_n;  // (compute program table below)',
                   'static int g_cs_n;  // (compute program table below)\nstatic void mk8_compute_prepare_reset(void);')
    text = replace(text, '    g_cs_n = 0;', '    mk8_compute_prepare_reset();\n    g_cs_n = 0;')
    text = replace(text, 'typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;',
                   'typedef struct CsProg { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; struct CsProg *mk8_retired_next; } CsProg;')
    text = replace(text, '    prog_state(&cp->p, r == VK_SUCCESS ? BUILD_READY : BUILD_FAILED);',
                   '    vkDestroyShaderModule(g_vk.dev, m, NULL);\n    prog_state(&cp->p, r == VK_SUCCESS ? BUILD_READY : BUILD_FAILED);')
    old = '''static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);'''
    new = '''static CsProg *mk8_compute_build(NvnShaderStage *ss, const uint8_t *owned_code, const uint32_t *local) {
    CsProg *cp = calloc(1, sizeof *cp);
    if (!cp) FATAL("Compute program allocation failed\\n");'''
    text = replace(text, old, new)
    text = replace(text, '    if (n < 256) { tab[n].addr = ss->code_addr; tab[n].cp = cp; g_cs_n = n + 1; }\n', '')
    text = replace(text, 'cp->si = maxwell_translate((const uint8_t *)(uintptr_t)ss->code_addr,',
                   'cp->si = maxwell_translate(owned_code ? owned_code : (const uint8_t *)(uintptr_t)ss->code_addr,')
    text = replace(text, 'cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;',
                   'cp->local[i] = local ? local[i] : ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;')
    boundary = 'static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {'
    text = replace(text, boundary, (source/'compute_prepare.inc').read_text(encoding='utf-8') + '\n' + boundary)
    execute.write_text(text, encoding='utf-8')
