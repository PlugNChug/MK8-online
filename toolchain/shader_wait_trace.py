"""Registra trabalhos de shader lentos; inventário completo somente por opt-in."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Shader wait trace boundary changed: ' + old[:100])
    return text.replace(old, new)


def prepare(runtime):
    path = runtime/'nvn/vulkan/exec.c'
    text = path.read_text(encoding='utf-8')
    support = (ROOT/'runtime/mk8/graphics/shader_wait_trace.inc').read_text(encoding='utf-8')
    text = replace(text, '} Prog;\n', '} Prog;\n' + support)
    text = replace(text, '    p->fkey = *fkey;\n', '''    p->fkey = *fkey;
    if (mk8_wait_trace_verbose())
        LOG("[mk8:shader-create] frame=%llu program=%p key=%llx vs=%llx fs=%llx gs=%llx\\n",
            (unsigned long long)vk_frame_counter(), (void *)p, (unsigned long long)h,
            (unsigned long long)p->code[0], (unsigned long long)p->code[1], (unsigned long long)p->code[2]);
''')
    text = replace(text, '    g_cs_creates++;\n', '''    g_cs_creates++;
    cp->p.code[0] = ss->code_addr;  // Identidade privada do compute para o diagnóstico.
    if (mk8_wait_trace_verbose())
        LOG("[mk8:shader-create] frame=%llu program=%p cs=%llx\\n",
            (unsigned long long)vk_frame_counter(), (void *)&cp->p, (unsigned long long)ss->code_addr);
''')
    text = replace(text, 'mk8_job_ready(g_tq, &p->tjob)',
                   'mk8_traced_job(g_tq, &p->tjob, p, "translate", 0, true)')
    text = replace(text, 'mk8_job_ready(g_wq, &p->cjob)',
                   'mk8_traced_job(g_wq, &p->cjob, p, "program", 0, true)')
    text = replace(text, 'mk8_job_ready(g_wq, &pp->job)',
                   'mk8_traced_job(g_wq, &pp->job, pr, "pipeline", ph, true)')
    text = replace(text, 'job_run_or_wait(g_wq, &cp->p.cjob);',
                   'mk8_traced_job(g_wq, &cp->p.cjob, &cp->p, "compute", 0, false);')
    text = replace(text, 'job_run_or_wait(g_tq, &pr->tjob);',
                   'mk8_traced_job(g_tq, &pr->tjob, pr, "prefetch-translate", 0, false);')
    for anchor in ['    size_t nv = 0, nf = 0;\n',
                   '    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);\n']:
        text = replace(text, anchor, '    uint64_t mk8_build_start = mk8_wait_trace_ms() > 0 ? SDL_GetTicksNS() : 0;\n' + anchor)
    text = replace(text, '    if (!sv || !sf || (p->gi && !sg)) {',
                   '    mk8_shader_build_time(p, &p->cjob, "program-spirv", mk8_build_start);\n    if (!sv || !sf || (p->gi && !sg)) {')
    text = replace(text, '    if (!spv) { prog_state(&cp->p, BUILD_FAILED); return; }',
                   '    mk8_shader_build_time(&cp->p, &cp->p.cjob, "compute-spirv", mk8_build_start);\n    if (!spv) { prog_state(&cp->p, BUILD_FAILED); return; }')
    for anchor, program, job, phase in [
        ('    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);', '&cp->p', '&cp->p.cjob', 'compute-driver'),
        ('    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe);', 'pp->k.prog', '&pp->job', 'graphics-driver'),
    ]:
        text = replace(text, anchor, '    uint64_t mk8_driver_start = mk8_wait_trace_ms() > 0 ? SDL_GetTicksNS() : 0;\n' + anchor +
                       f'\n    mk8_shader_build_time({program}, {job}, "{phase}", mk8_driver_start);')
    path.write_text(text, encoding='utf-8')
    path = runtime/'nvn/objects.c'
    text = path.read_text(encoding='utf-8')
    anchor = '    if ((p->stage_mask & 3) == 3) {  // (compile-ahead: a program set now is often drawn much later; shader_ahead.h)\n'
    text = replace(text, anchor, '''    extern bool mk8_wait_trace_verbose(void);
    if (mk8_wait_trace_verbose()) {
        extern uint64_t renderer_present_count(void);
        LOG("[mk8:shader-set] frame=%llu mask=%x vs=%llx fs=%llx gs=%llx cs=%llx\\n",
            (unsigned long long)renderer_present_count(), p->stage_mask,
            (unsigned long long)p->stages[0].code_addr, (unsigned long long)p->stages[1].code_addr,
            (unsigned long long)p->stages[2].code_addr, (unsigned long long)p->stages[5].code_addr);
    }
''' + anchor)
    path.write_text(text, encoding='utf-8')
