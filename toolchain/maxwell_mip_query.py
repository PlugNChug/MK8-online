"""Resolve consultas TMML.B de efeitos MK8 pela tabela estática de texturas NVN."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def prepare(runtime):
    path = runtime/'nvn/maxwell/translate.c'
    text = path.read_text(encoding='utf-8')
    boundary = 'static void emit_insn(Ctx *c, uint64_t w, uint32_t pc) {'
    opcode = '    } else if (OPM(0xdf58000000000000ull, 0xfff8000000000000ull)) {  // TMML'
    count = '    c->code = code;  // (emit_insn looks at neighbouring words for a few patterns)'
    if any(text.count(anchor) != 1 for anchor in (boundary, opcode, count)) or 'mk8_emit_tmml_bindless' in text:
        raise ValueError('TMML.B translator boundary changed')
    helper = (ROOT/'runtime/mk8/graphics/maxwell_mip_query.inc').read_text(encoding='utf-8')
    text = text.replace(boundary, helper+'\n'+boundary)
    text = text.replace(count, count+'\n    c->ninsts = end;')
    text = text.replace(opcode,
        '    } else if (OPM(0xdf60000000000000ull, 0xfff8000000000000ull)) {  // TMML.B\n'
        '        mk8_emit_tmml_bindless(c, w, pc);\n'+opcode)
    path.write_text(text, encoding='utf-8')
