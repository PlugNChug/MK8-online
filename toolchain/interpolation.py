"""Respeita a interpolação declarada no cabeçalho SPH dos fragment shaders."""


def prepare(runtime):
    path = runtime/'nvn/maxwell/translate.c'
    text = path.read_text(encoding='utf-8')
    changes = [
        ('        c->omap_target = *(const uint32_t *)(sph + 0x48);', '''        // SPH: quatro componentes de dois bits por atributo; zero indica componente sem uso.
        for (int a = 0; a < 32; a++) {
            uint8_t imap = sph[0x18 + a];
            int type = 0;
            for (int comp = 0; comp < 4 && !type; comp++) type = (imap >> (2 * comp)) & 3;
            if (type == 1) c->flat_mask |= 1u << a;
            if (type == 3) c->noperspective_mask |= 1u << a;
        }
        c->omap_target = *(const uint32_t *)(sph + 0x48);'''),
        ('                if (c->ipa_mode == 0) sprintf(e,',
         '                if (c->ipa_mode == 0 && !((c->flat_mask | c->noperspective_mask) & (1u << a))) sprintf(e,'),
        ('                    sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w * as_type<float>(%s))", a, comp, reg(c, c->ipa_mul, mb));',
         '''                    if ((c->flat_mask | c->noperspective_mask) & (1u << a))
                        sprintf(e, "as_type<uint>(in.v%d[%d] * as_type<float>(%s))", a, comp, reg(c, c->ipa_mul, mb));
                    else sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w * as_type<float>(%s))", a, comp, reg(c, c->ipa_mul, mb));'''),
    ]
    for old, new in changes:
        if text.count(old) != 1:
            raise ValueError('Shader interpolation boundary changed')
        text = text.replace(old, new)
    path.write_text(text, encoding='utf-8')
