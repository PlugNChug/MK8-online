// Guest event calls can yield the logical core. Never hold a host mutex across them:
// another guest on that core would block on this mutex, preventing its owner resuming.
static void mk8_movie_clear_event(Cpu *c, uint64_t event) {
    uint64_t saved[8];
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = event;
    guest_call(c, guest_sym("_ZN2nn2os10ClearEventEPNS0_9EventTypeE"));
    memcpy(c->x, saved, sizeof saved);
}

static void dec_signal(Cpu *c, Decoder *d, int i) {
    pthread_mutex_lock(&d->lock);
    uint64_t event = d->stopped ? 0 : d->events[i];
    pthread_mutex_unlock(&d->lock);
    if (!event) return;
    signal_guest_event(c, event);
    pthread_mutex_lock(&d->lock);
    bool stopped = d->stopped;
    pthread_mutex_unlock(&d->lock);
    // Stop may run while SignalEvent yields; do not leave a late event armed.
    if (stopped) mk8_movie_clear_event(c, event);
}

static void dec_stop(Cpu *c) {
    Decoder *d = self_state(c);
    uint64_t events[4] = {0};
    pthread_mutex_lock(&d->lock);
    if (!d->stopped) {
        d->stopped = true;
        memcpy(events, d->events, sizeof events);
        LOG("[movie] decoder %s stopped\n", d->video ? "video" : "audio");
    }
    pthread_mutex_unlock(&d->lock);
    for (int i = 0; i < 4; ++i)
        if (events[i]) mk8_movie_clear_event(c, events[i]);
    c->x[0] = 0;
}
