"""Without MAILBOX, present late frames at once (FIFO_RELAXED) instead of holding them for the next vblank.

The pinned runtime asks for MAILBOX and falls back to plain FIFO. AMD's Windows driver (RX 580, 26.1.1) offers
only IMMEDIATE, FIFO and FIFO_RELAXED, so AMD players ran on FIFO, where a frame a little over 16.7 ms waits for the
following vblank. FIFO_RELAXED keeps vsync for frames on time and shows a late one right away. On the RX 580, where
race frames are rarely late, two runs of each mode measured the same (14/11/2026: 60 FPS, p99 17.6 ms); the gain is
for machines that miss the budget often, not shown yet. The mode was not logged before, so player reports could not
tell it. The order is MAILBOX, FIFO_RELAXED, FIFO; LARECOMP_VK_PRESENT=fifo|relaxed|immediate|mailbox still chooses
(falling back to FIFO when the surface lacks it). The chosen mode is logged once per change.
"""

OLD = '''    const char *e = getenv("LARECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}
'''

NEW = '''    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    VkPresentModeKHR chosen = mk8_pick_present_mode(m, n, getenv("LARECOMP_VK_PRESENT"));  // MK8 (present_mode.py)
    static int logged = -1;
    if ((int)chosen != logged) {
        logged = (int)chosen;
        LOG("[vk] present mode %s (%u offered)\\n", chosen == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
            : chosen == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "fifo-relaxed"
            : chosen == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo", n);
    }
    return chosen;
}
'''

HELPER_BOUNDARY = 'static VkPresentModeKHR present_mode(void) {\n'

HELPER = '''// MK8 (toolchain/present_mode.py): the requested mode when the surface offers it; by default MAILBOX, then
// FIFO_RELAXED (a late frame is shown at once instead of waiting a whole vblank), then FIFO (always offered).
static VkPresentModeKHR mk8_pick_present_mode(const VkPresentModeKHR *modes, uint32_t n, const char *request) {
    VkPresentModeKHR order[2] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR};
    uint32_t count = 2;
    if (request && *request) {
        count = 1;
        order[0] = !strcmp(request, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                 : !strcmp(request, "relaxed") ? VK_PRESENT_MODE_FIFO_RELAXED_KHR
                 : !strcmp(request, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    }
    for (uint32_t k = 0; k < count; k++)
        for (uint32_t i = 0; i < n; i++)
            if (modes[i] == order[k]) return order[k];
    return VK_PRESENT_MODE_FIFO_KHR;
}

'''


def prepare(runtime):
    device = runtime/'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
    for boundary in (OLD, HELPER_BOUNDARY):
        if text.count(boundary) != 1:
            raise ValueError(f'Present mode boundary changed: {boundary[:80]}')
    text = text.replace(OLD, NEW).replace(HELPER_BOUNDARY, HELPER + HELPER_BOUNDARY)
    device.write_text(text, encoding='utf-8')
