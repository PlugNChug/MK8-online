"""Share unused integer argument slots with floating-point command arguments."""


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Compact command layout boundary changed: {old[:90]}')
    return text.replace(old, new)


def prepare(runtime):
    header = runtime/'nvn/nvn.h'
    commands = runtime/'nvn/cmdbuf.c'
    text = header.read_text(encoding='utf-8')
    text = replace(text, '''    uint64_t x[7];      // x1..x7 of the call
    float f[4];         // s0..s3 of the call
''', '''    // Float-bearing commands use at most x1..x3; x6/x7 can share their storage.
    union {
        uint64_t x[7];
        struct { uint64_t mk8_float_padding[5]; float f[4]; };
    };
''')
    text = replace(text, '} CmdRec;', '''} CmdRec;
_Static_assert(sizeof(CmdRec) == 72, "Recheck compact command layout and consumers");''')
    source = commands.read_text(encoding='utf-8')
    source = replace(source, '    memcpy(r->f, s->f, sizeof r->f);', '''    switch (op) {
    case CMD_SET_ALPHA_REF: case CMD_SET_DEPTH_BOUNDS: case CMD_SET_POLYGON_OFFSET:
    case CMD_SET_LINE_WIDTH: case CMD_SET_POINT_SIZE: case CMD_CLEAR_DEPTH_STENCIL:
        memcpy(r->f, s->f, sizeof r->f);
        break;
    default: break;
    }''')
    header.write_text(text, encoding='utf-8')
    commands.write_text(source, encoding='utf-8')
