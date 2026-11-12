"""Libera condições de threads guest já unidas, preservando seus objetos e handles guest."""

BOUNDARIES = (
    ('static void wake(KThread *t) { pthread_cond_signal(&t->cond); }',
     'static void wake(KThread *t) { if (!t->finished) pthread_cond_signal(&t->cond); }'),
    ('    for (int i = 0; i < n; i++) pthread_join(done[i]->host, NULL);  // (past `finished`: only its return is left)',
     '''    for (int i = 0; i < n; i++) {
        int err = pthread_join(done[i]->host, NULL);
        if (err) FATAL("MK8: join de thread finalizada falhou: %s\\n", strerror(err));
        // O objeto continua sinalizado para handles guest; nenhuma thread host usa mais esta condição.
        err = pthread_cond_destroy(&done[i]->cond);
        if (err) FATAL("MK8: liberação da condição de thread falhou: %s\\n", strerror(err));
    }'''),
    ('        pthread_cond_destroy(&t->cond);\n        free(t);',
     '        if (!t->joined) pthread_cond_destroy(&t->cond);\n        free(t);'),
)


def prepare(runtime):
    kernel = runtime/'kernel.c'
    text = kernel.read_text(encoding='utf-8')
    for old, _ in BOUNDARIES:
        if text.count(old) != 1:
            raise ValueError('Thread condition lifetime boundary changed')
    for old, new in BOUNDARIES:
        text = text.replace(old, new)
    kernel.write_text(text, encoding='utf-8')
