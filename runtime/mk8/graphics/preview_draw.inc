// Called only inside the explicit F7 window, on the command-translation owner.
static void mk8_preview_command(const CmdRec *r, bool indexed) {
    Mk8PreviewDraw d={.frame=vk_frame_counter()+1,.count=(uint32_t)r->x[2],.mode=(uint32_t)r->x[0],
        .indexed=indexed,.render_off=st.render_off,.cull=st.poly[0],
        .depth_state=(st.ds[0]!=0) | ((st.ds[1]!=0)<<1) | ((st.ds[2]&255u)<<8)};
    if (st.program) { d.vs=st.program->stages[0].code_addr; d.fs=st.program->stages[1].code_addr; }
    d.geometry_token=mk8_gfx_token(st.vbuf[0].addr ^ (indexed ? r->x[3]<<1 : 0));
    memcpy(d.viewport,st.viewport[0],sizeof d.viewport);
    memcpy(d.scissor,st.scissor[0],sizeof d.scissor);
    const NvnTexture *color=st.rt.ncolor>0 ? st.rt.color[0] : NULL;
    const TexBackend *cb=color ? color->backend : NULL;
    const TexBackend *db=st.rt.depth ? st.rt.depth->backend : NULL;
    if (cb) { d.color_w=cb->width; d.color_h=cb->height; }
    if (db) { d.depth_w=db->width; d.depth_h=db->height; }
    mk8_gfx_preview_draw(&d);
}
