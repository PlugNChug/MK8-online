"""SAMPLES_PASSED counters (NVN occlusion reports) measured with Vulkan occlusion queries.

Upstream writes 0 for every counter value, so every occlusion-tested object looked hidden: MK8 4.0.1 racers vanished
at a distance and appeared only up close (user report, 06/11/2026), and light glows follow the same reports (a GP run
answering "visible" for all of them came up blown out). NVN's counter is cumulative: each report holds the samples
passed so far, and the game takes differences. Here each report ends the open occlusion query and begins the next
(outside render passes, which a query may span), and notes how many queries of its submission precede it. When the
submission is done the results are added in submission order to a running total and each report gets the total at
its point. Conditional rendering resolves pending reports for either operand before reading them; otherwise a
first use could compare initial zeros and suppress visible objects. MK8_OCCLUSION_VISIBLE=1 (diagnostic) answers "visible".
"""

STATE = '''
// MK8 (toolchain/occlusion.py): SAMPLES_PASSED reports measured with occlusion queries, per submission slot
#define MK8_OQ_MAX 256
extern VkQueryControlFlags mk8_occlusion_flags;
typedef struct {
    VkQueryPool pool;
    uint32_t n, nrep;            // queries begun, reports recorded
    uint32_t dirty;              // used prefix, retained after reading the results
    uint64_t seq;                // the submission
    bool active;                 // query n - 1 still open
    uint64_t *rep_out[MK8_OQ_MAX];
    uint32_t rep_after[MK8_OQ_MAX];  // queries of this submission before the report
} Mk8Oq;
static Mk8Oq mk8_oq[SUBS];
static uint64_t mk8_samples_total;  // samples passed in every resolved query (cumulative, as NVN's counter)
static bool mk8_oq_carry;  // a query was open when its command buffer was submitted: the next one goes on measuring
static void mk8_oq_reset_pool(VkCommandBuffer cb, Mk8Oq *o) {
    if (o->pool && o->dirty) vkCmdResetQueryPool(cb, o->pool, 0, o->dirty);
    o->dirty = 0;
}
static void mk8_oq_resolve(Mk8Oq *o) {  // (cq_lock held) its submission is done
    uint64_t r[MK8_OQ_MAX] = {0};
    bool ok = o->n && o->pool &&
              vkGetQueryPoolResults(g_vk.dev, o->pool, 0, o->n, sizeof r, r, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    if (o->n && !ok) FATAL("[mk8] completed occlusion queries unavailable (submission %llu)\\n", (unsigned long long)o->seq);
    uint64_t base = mk8_samples_total, sum = 0;
    uint32_t q = 0;
    for (uint32_t k = 0; k < o->nrep; k++) {
        for (; q < o->rep_after[k] && q < o->n; q++) sum += ok ? r[q] : 0;
        if (o->rep_out[k]) { o->rep_out[k][0] = base + sum; o->rep_out[k][1] = ns_to_gpu_ticks(plat_now_ns()); }
        else { base = 0; sum = 0; }  // ResetCounter at this point in GPU order.
    }
    for (; q < o->n; q++) sum += ok ? r[q] : 0;  // (after the last report: still counted, as the hardware counter)
    mk8_samples_total = base + sum;
    o->n = o->nrep = 0;
    o->active = false;
}
static void mk8_oq_resolve_done(uint64_t done) {  // (cq_lock held) in submission order: the total is cumulative
    for (;;) {
        Mk8Oq *next = NULL;
        for (int i = 0; i < SUBS; i++)
            if ((mk8_oq[i].n || mk8_oq[i].nrep) && mk8_oq[i].seq <= done && (!next || mk8_oq[i].seq < next->seq)) next = &mk8_oq[i];
        if (!next) return;
        mk8_oq_resolve(next);
    }
}
static uint64_t mk8_oq_pending_pair(const uint64_t *a, const uint64_t *b) {
    uint64_t seq = 0;
    pthread_mutex_lock(&cq_lock);
    for (int i = 0; i < SUBS; i++) {
        const Mk8Oq *o = &mk8_oq[i];
        for (uint32_t k = 0; k < o->nrep; k++)
            if (o->rep_out[k] && (o->rep_out[k] == a || o->rep_out[k] == b)) {
                if (!o->seq) FATAL("[mk8] pending occlusion report without submission\\n");
                if (o->seq > seq) seq = o->seq;
            }
    }
    pthread_mutex_unlock(&cq_lock);
    return seq;
}
void renderer_flush(void);
static void mk8_oq_wait_pair(const uint64_t *q) {  // renderer owner, before either CPU operand load
    if (!q) return;
    uint64_t seq = mk8_oq_pending_pair(q, q + 2);
    if (!seq) return;  // no report dependency: no submission or wait
    uint64_t pending = vk_pending_seq();
    if (seq > pending) FATAL("[mk8] occlusion report beyond pending submission\\n");
    wait_why = "conditional samples";
    // Never hold cq_lock here: submission retirement and waits resolve reports under the same lock.
    if (seq == pending) renderer_flush();  // its producer is still being recorded: submit it first
    else vk_wait_seq(seq);                // already submitted: leave unrelated recorded work alone
    // vk_wait_seq logs and returns on timeout. A stale report must not become a visibility decision.
    if (mk8_oq_pending_pair(q, q + 2)) FATAL("[mk8] conditional occlusion report did not resolve\\n");
}
'''

CONDITION_OLD = '                bool eq = q ? q[0] == q[2] : true;'
CONDITION_NEW = '                mk8_oq_wait_pair(q);\n' + CONDITION_OLD

RESOLVE_OLD = '    for (int i = 0; i < SUBS; i++) if (cq[i].n && cq[i].seq <= done) cq_resolve(i);\n    pthread_mutex_unlock(&cq_lock);\n'
RESOLVE_NEW = '    for (int i = 0; i < SUBS; i++) if (cq[i].n && cq[i].seq <= done) cq_resolve(i);\n    mk8_oq_resolve_done(done);\n    pthread_mutex_unlock(&cq_lock);\n'

BEGIN_OLD = '        gp_mark(s->main, "(submission start; includes GPU idle before it)");\n'
BEGIN_NEW = '''        {   // MK8: this slot's occlusion queries (reset outside any render pass; resolved when the slot was freed: sub())
            Mk8Oq *o = &mk8_oq[cur_sub];
            if (!o->pool) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION,
                                            .queryCount = MK8_OQ_MAX};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &o->pool) != VK_SUCCESS) o->pool = VK_NULL_HANDLE;
                else o->dirty = MK8_OQ_MAX;  // First use requires resetting every query.
            }
            pthread_mutex_lock(&cq_lock);
            o->n = o->nrep = 0;
            o->active = false;
            o->seq = s->seq;
            pthread_mutex_unlock(&cq_lock);
            mk8_oq_reset_pool(s->main, o);
            if (o->pool && mk8_oq_carry) {  // (between two reports a submission may end: e.g. a compute result wait)
                vkCmdBeginQuery(s->main, o->pool, 0, mk8_occlusion_flags);
                o->n = 1;
                o->dirty = 1;
                o->active = true;
            }
            mk8_oq_carry = false;
        }
''' + BEGIN_OLD

SUBMIT_OLD = '    vk_cmd();  // (an empty main buffer still carries the semaphores)\n'
SUBMIT_NEW = SUBMIT_OLD + '''    {   // MK8: an occlusion query can't outlive its command buffer (its samples still count toward the total)
        Mk8Oq *o = &mk8_oq[cur_sub];
        if (o->active) { vkCmdEndQuery(s->main, o->pool, o->n - 1); o->active = false; mk8_oq_carry = true; }
    }
'''

REPORT_OLD = '            if (!out) break;\n            out[0] = 0;\n'
REPORT_NEW = '''            bool reset_samples = r->x[0] == 0x100000001ull;
            if (!out && !reset_samples) break;
            static int mk8_occl_zero = -1;  // (MK8_OCCLUSION_ZERO=1: upstream's 0, for comparisons)
            if (mk8_occl_zero < 0) mk8_occl_zero = getenv("MK8_OCCLUSION_ZERO") != NULL;
            if (type == 1 && !mk8_occl_zero) {  // SAMPLES_PASSED (MK8: toolchain/occlusion.py)
                static int visible = -1;
                static uint64_t reports;
                static bool saw_reset;
                if (reset_samples && !saw_reset) {
                    saw_reset = true;
                    LOG("[mk8] samples-passed reset recorded\\n");
                }
                if (visible < 0) visible = getenv("MK8_OCCLUSION_VISIBLE") != NULL;
                if (!reports++ || reports % 20000 == 0)
                    LOG("[mk8] samples-passed reports: %llu (%s)\\n", (unsigned long long)reports, visible ? "diagnostic: visible" : "occlusion queries");
                if (visible) {
                    static uint64_t samples;
                    if (reset_samples) samples = 0;
                    else {
                        out[0] = (samples += 1u << 20);
                        out[1] = ns_to_gpu_ticks(plat_now_ns());
                    }
                } else {
                    vk_end_pass();  // (queries are begun and ended outside render passes: they may span several)
                    VkCommandBuffer qcb = vk_cmd();
                    Mk8Oq *o = &mk8_oq[cur_sub];
                    pthread_mutex_lock(&cq_lock);
                    // (nothing written now: the GPU writes a report when it is past it - here at resolve, value and
                    // time together; a provisional value mixed with a resolved one made glows huge, boots 214/216)
                    if (o->pool && o->nrep < MK8_OQ_MAX) {
                        if (o->active) { vkCmdEndQuery(qcb, o->pool, o->n - 1); o->active = false; }
                        o->rep_out[o->nrep] = reset_samples ? NULL : out;
                        o->rep_after[o->nrep++] = o->n;
                        if (o->n < MK8_OQ_MAX) {
                            vkCmdBeginQuery(qcb, o->pool, o->n++, mk8_occlusion_flags);
                            o->dirty = o->n;
                            o->active = true;
                        }
                    }
                    pthread_mutex_unlock(&cq_lock);
                }
                break;  // (upstream's CPU-time write below is for the other counters)
            }
            if (reset_samples) break;
            out[0] = 0;
'''


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Occlusion boundary changed: {old.strip()[:80]}')
    return text.replace(old, new)


def prepare(runtime):
    commands = runtime/'nvn/cmdbuf.c'
    recorded = commands.read_text(encoding='utf-8')
    recorded = replace(recorded, 'NVN(nvnCommandBufferResetCounter) { (void)c; }', '''NVN(nvnCommandBufferResetCounter) {
    if ((uint32_t)A(1) != 1) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_REPORT_COUNTER, 0, &s);
    r->x[0] = 0x100000001ull;  // Internal marker: reset SAMPLES_PASSED, no guest result address.
    r->x[1] = 0;
}''')
    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    anchor = 'static void cq_resolve_done(uint64_t done) {  // every slot whose submission is done\n'
    text = replace(text, anchor, STATE + anchor)
    text = replace(text, RESOLVE_OLD, RESOLVE_NEW)
    text = replace(text, BEGIN_OLD, BEGIN_NEW)
    text = replace(text, SUBMIT_OLD, SUBMIT_NEW)
    text = replace(text, REPORT_OLD, REPORT_NEW)
    text = replace(text, CONDITION_OLD, CONDITION_NEW)
    device = runtime/'nvn/vulkan/device.c'
    setup = device.read_text(encoding='utf-8')
    setup = replace(setup, 'VkG g_vk;\n', 'VkG g_vk;\nVkQueryControlFlags mk8_occlusion_flags;\n')
    features = '    b->independentBlend = h->independentBlend;\n'
    setup = replace(setup, features, features + '''    // NVN consumes sample counts, so request exact counts when the device can provide them.
    b->occlusionQueryPrecise = h->occlusionQueryPrecise && !getenv("MK8_OCCLUSION_APPROXIMATE");
    mk8_occlusion_flags = b->occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
    LOG("[mk8] occlusion counts: %s\\n", mk8_occlusion_flags ? "precise" : "approximate");
''')
    execute.write_text(text, encoding='utf-8')
    commands.write_text(recorded, encoding='utf-8')
    device.write_text(setup, encoding='utf-8')
