"""Translate the fixed NVN point size for point lists whose vertex shader supplies none."""


def edit(path, replacements):
    text = path.read_text(encoding='utf-8')
    for old, new in replacements:
        if text.count(old) != 1:
            raise ValueError(f'Point size preparation boundary changed in {path.name}: {old[:60]}')
        text = text.replace(old, new)
    path.write_text(text, encoding='utf-8')


def prepare(runtime):
    vk = runtime/'nvn/vulkan'
    prelude = '    sp(&s, "%s%s%s", glsl_prelude, maxwell_glsl_bda ? glsl_prelude_bda : "", glsl_prelude_defs);'
    init = r'        if (info->writes_point_size) sp(&s, "  gl_PointSize = 0.0f;\n");'
    edit(runtime/'nvn/maxwell/translate.c', [
        (prelude, prelude+r'''
    if (stage == STAGE_VERTEX && !info->writes_point_size)
        sp(&s, "layout(constant_id = 0) const float fixedPointSize = 1.0f;\n");
'''),
        (init, init + r'''
        else sp(&s, "  gl_PointSize = fixedPointSize;\n");'''),
    ])
    edit(vk/'vk.h', [('    bool bda;', '    bool large_points;\n    bool bda;')])
    enabled = '    b->largePoints = h->largePoints;'
    edit(vk/'device.c', [(enabled, enabled+'\n    g_vk.large_points = b->largePoints;')])
    fill = '    k->topo_class = (uint8_t)topo_class;'
    key = 'static void pipe_key(const ExecState *s,'
    stages = '    VkVertexInputAttributeDescription va[32];'
    mode = '        case CMD_SET_POLYGON_OFFSET: memcpy(s->poly_offset, r->f, sizeof s->poly_offset); return true;'
    edit(vk/'exec.c', [
        ('    float blend_color[4];', '    float fixed_point_size;\n    float blend_color[4];'),
        ('    st.channel_mask = 0xFFFFFFFF;', '    st.fixed_point_size = 1.0f;\n    st.channel_mask = 0xFFFFFFFF;'),
        (mode, '        case CMD_SET_POINT_SIZE: s->fixed_point_size = r->f[0]; return true;\n'+mode),
        ('    uint32_t write_mask[8];\n} PipeKey;', '    uint32_t write_mask[8];\n    float fixed_point_size;\n} PipeKey;'),
        (key, '''static float fixed_point_key(int topo_class, bool geometry, bool shader_size, float size,
                             bool large_points, const VkPhysicalDeviceLimits *limits) {
    // Zero leaves the vertex specialization at its default of one. Non-point draws share one key.
    if (topo_class != 0 || geometry || shader_size || !large_points || !(size > 0.0f) || !isfinite(size) || size == 1.0f)
        return 0.0f;
    float lo = limits->pointSizeRange[0], hi = limits->pointSizeRange[1];
    size = fminf(fmaxf(size, lo), hi);
    float step = limits->pointSizeGranularity;
    if (step > 0.0f)
        size = fminf(hi, lo + roundf((size - lo) / step) * step);
    return size == 1.0f ? 0.0f : size;
}

'''+key),
        (fill, fill+'''
    k->fixed_point_size = fixed_point_key(topo_class, pr->gi != NULL, pr->vi->writes_point_size,
                                          s->fixed_point_size, g_vk.large_points, &g_vk.props.limits);'''),
        (stages, '''    VkSpecializationMapEntry point_entry = {.constantID = 0, .offset = 0, .size = sizeof(float)};
    VkSpecializationInfo point_spec = {.mapEntryCount = 1, .pMapEntries = &point_entry,
                                       .dataSize = sizeof(float), .pData = &k->fixed_point_size};
    if (k->fixed_point_size != 0.0f) stages[0].pSpecializationInfo = &point_spec;
'''+stages),
    ])
