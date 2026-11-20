"""Carry NVN depth clamping through Vulkan execution and pipeline prefetch."""


def prepare(runtime):
    changes = {
        'nvn/vulkan/exec.c': [
            ('    bool prim_restart;', '    bool prim_restart, depth_clamp;'),
            ('    uint8_t blend_on, a2c, topo_class, poly_fill;',
             '    uint8_t blend_on, a2c, topo_class, poly_fill, depth_clamp;'),
            ('    k->poly_fill = s->poly[2] != 1;',
             '    k->poly_fill = s->poly[2] != 1;\n    k->depth_clamp = s->depth_clamp;'),
            ('    PipeKey *k = &pp->k;',
             '    PipeKey *k = &pp->k;\n'
             '    if (k->depth_clamp && !g_vk.depth_clamp)\n'
             '        FATAL("[vk] Game requested depth clamping, but this GPU does not support depthClamp\\n");'),
            ('                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};',
             '                                                 .depthClampEnable = k->depth_clamp,\n'
             '                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};'),
            ('        case CMD_SET_POLYGON_OFFSET: memcpy(s->poly_offset, r->f, sizeof s->poly_offset); return true;',
             '        case CMD_SET_DEPTH_CLAMP: s->depth_clamp = (r->x[0] & 1) != 0; return true;\n'
             '        case CMD_SET_POLYGON_OFFSET: memcpy(s->poly_offset, r->f, sizeof s->poly_offset); return true;'),
        ],
        'nvn/vulkan/vk.h': [
            ('    bool custom_border, divisor, host_import, geometry;',
             '    bool custom_border, divisor, host_import, geometry, depth_clamp;'),
        ],
        'nvn/vulkan/device.c': [
            ('    b->depthClamp = h->depthClamp;',
             '    b->depthClamp = h->depthClamp;\n    g_vk.depth_clamp = b->depthClamp;'),
        ],
    }
    for relative, edits in changes.items():
        path = runtime/relative
        text = path.read_text(encoding='utf-8')
        for before, after in edits:
            if text.count(before) != 1:
                raise ValueError('Vulkan depth-clamp boundary changed: '+relative)
            text = text.replace(before, after, 1)
        path.write_text(text, encoding='utf-8')
