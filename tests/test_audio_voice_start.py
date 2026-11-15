"""Exercise the prepared mixer on synthetic voices; no game audio or SDL device required."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER


def between(text, start, end):
    at = text.index(start)
    return text[at:text.index(end, at)]


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class AudioVoiceStartTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        out = Path(cls.folder.name)
        runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out)
        text = (runtime/'services/audio.c').read_text(encoding='utf-8')
        types = between(text, '#define WB_COUNT', '// ------------------------------------------------------------------ a stream played as another')
        decode = between(text, 'static void mark_end(', '// ------------------------------------------------------------------ reverb')
        render = between(text, 'static int hf_log', '// Audio > Mute in background')
        splitter = between(text, 'static Mix *spl_target(Renderer *r, uint32_t splitter, int k, SplDest **out) {',
                           'static void request_update(')
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef int pthread_mutex_t, pthread_t;
typedef void KEvent, SDL_AudioStream;
#define LOG(...) ((void)0)
''' + types + r'''
static int g_nswap;
static bool g_swap_on;
static int64_t g_swap_pos;
static uint64_t g_swap_done;
#define SWAP_FADE 48000
typedef struct { int16_t *pcm; int64_t frames; int stride; float mix; } SwapChan;
static void swap_voice_new(Voice *v) { abort(); }
static SwapChan *swap_for(Voice *v, WaveBuf *wb, int64_t *base) { *base=-1; return NULL; }
''' + decode + splitter + r'''
static void apply_effects(Renderer *r, int32_t id, Mix *m) { assert(r->effects==0); }
''' + render + r'''
#define N 240
static Voice voices[2];
static ChanRes resources[2];
static ChanState states[2];
static Mix mixes[1];
static float buffers[MIXBUF_MAX*N], output[N*2];
static int16_t pcm[8192];
static uint8_t adpcm[4096];
static int16_t coefficients[16], context[3];
static Splitter split;
static SplDest destination;
static Renderer r;
static void init(unsigned channels,bool use_splitter) {
    Voice *voice=&voices[0];
    memset(voices,0,sizeof voices); memset(resources,0,sizeof resources); memset(states,0,sizeof states);
    memset(mixes,0,sizeof mixes); memset(&r,0,sizeof r); memset(&destination,0,sizeof destination);
    for (unsigned i=0;i<8192;i++) pcm[i]=channels==2 && (i&1) ? -8192 : 16384;
    r.sample_rate=48000; r.sample_count=N; r.voices=channels; r.voice=voices;
    r.chres=resources; r.chst=states; r.mix=mixes; r.nmix=1;
    r.sink_used=1; r.sink_inputs=2; r.sink_input[0]=0; r.sink_input[1]=1;
    mixes[0].used=1; mixes[0].volume=1; mixes[0].bufs=2; mixes[0].buf=buffers;
    mixes[0].dest=-1; mixes[0].dest_splitter=UINT32_MAX;
    voice->in_use=voice->is_new=1; voice->play_state=0; voice->fmt=2; voice->chans=channels;
    voice->rate=48000; voice->pitch=voice->volume=1; voice->prev_volume=.7f;
    voice->wb_count=1; voice->mix_id=0; voice->splitter_id=UINT32_MAX;
    for (unsigned i=0;i<WB_COUNT;i++) voice->wb[i].sent=1;
    voice->wb[0]=(WaveBuf){.addr=(uintptr_t)pcm,.size=sizeof pcm,.end=8192/channels,.sent=0};
    for (unsigned c=0;c<channels;c++) {
        voice->chan_ids[c]=c;
        // A new voice can reuse routing resources whose previous gain is already nonzero.
        resources[c].mix[c]=resources[c].prev[c]=1;
        states[c].hist[0]=1000; states[c].fraction=.75f; states[c].h0=123;
    }
    if (channels==1) resources[0].mix[1]=resources[0].prev[1]=1;
    if (use_splitter) {
        r.splitters=r.spl_dests=1; r.spl=&split; r.sdst=&destination;
        split.ndest=1; split.dest[0]=0; destination.used=1; destination.mix=0;
        destination.vol[0]=destination.vol[1]=destination.prev[0]=destination.prev[1]=1;
        voice->mix_id=INT32_MAX; voice->splitter_id=0;
    }
}
static void near(float a,float b) { assert(fabsf(a-b)<.000003f); }
static void first_block(unsigned channels,float level) {
    render_frame(&r,output);
    for (unsigned i=0;i<N;i++) {
        float ramp=i<3 ? 0 : (float)i/N;
        near(output[i*2],level*ramp);
        near(output[i*2+1],(channels==2 ? -.25f : level)*ramp);
    }
    near(voices[0].prev_volume,voices[0].volume);
}
int main(int argc,char **argv) {
    assert(argc==2);
    bool stereo=!strcmp(argv[1],"stereo"), splitter=!strcmp(argv[1],"splitter");
    init(stereo?2:1,splitter);
    Voice *voice=&voices[0];
    if (!strncmp(argv[1],"pitch32",7)) {
        float pitch=!strcmp(argv[1],"pitch32slow") ? 1.f : 1.498f;
        float unramped[N*2];
        voice->rate=32000; voice->pitch=pitch; voice->is_new=0; voice->prev_volume=1;
        memset(states,0,sizeof states);
        render_frame(&r,unramped);
        init(1,false); voice->rate=32000; voice->pitch=pitch;
        render_frame(&r,output);
        for (unsigned i=0;i<N;i++) {
            near(output[i*2],unramped[i*2]*(float)i/N);
            near(output[i*2+1],unramped[i*2+1]*(float)i/N);
        }
        render_frame(&r,output); for (unsigned i=0;i<N*2;i++) near(output[i],.5f);
        return 0;
    }
    if (!strcmp(argv[1],"loop")) {
        voice->wb[0].end=20; voice->wb[0].loop=1; voice->wb[0].loop_count=-1;
    }
    if (!strcmp(argv[1],"late")) {
        voice->wb[0].sent=1;
        render_frame(&r,output);
        for (unsigned i=0;i<N*2;i++) near(output[i],0);
        voice->wb[0].sent=0;
    }
    if (!strcmp(argv[1],"adpcm")) {
        for (unsigned i=0;i<sizeof adpcm;i++) adpcm[i]=(i%8)==0 ? 0 : 0x77;
        voice->fmt=6; voice->ds_addr=(uintptr_t)coefficients;
        voice->wb[0]=(WaveBuf){.addr=(uintptr_t)adpcm,.size=sizeof adpcm,.end=7000,.ctx=(uintptr_t)context};
        first_block(1,7.f/32768); return 0;
    }
    first_block(stereo?2:1,.5f);
    for (unsigned k=0;k<3;k++) {
        render_frame(&r,output);
        for (unsigned i=0;i<N;i++) { near(output[i*2],.5f); near(output[i*2+1],stereo?-.25f:.5f); }
    }
    if (!stereo && !splitter) {
        voice->volume=.5f; render_frame(&r,output);
        for (unsigned i=0;i<N;i++) near(output[i*2],.5f-.25f*i/N);
        voice->volume=0; render_frame(&r,output);
        for (unsigned i=0;i<N;i++) near(output[i*2],.25f*(1.f-(float)i/N));
        render_frame(&r,output); for (unsigned i=0;i<N*2;i++) near(output[i],0);
        voice->is_new=1; voice->volume=1; voice->wb[0].sent=0;
        first_block(1,.5f); // restarted/reused voice gets its own initial ramp
    }
    return 0;
}
'''
        program = out/'audio-start.c'
        program.write_text(source, encoding='utf-8')
        cls.exe = out/'audio-start.exe'
        env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
        result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2',
                                 '-UNDEBUG', str(program), '-o', str(cls.exe)],
                                env=env, capture_output=True, text=True)
        if result.returncode:
            cls.folder.cleanup()
            raise RuntimeError(result.stderr[-4000:])

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def run_case(self, name):
        result = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_new_reused_voice_ramps_then_steady_volume_and_mute(self):
        self.run_case('mono')

    def test_stereo_channels_share_the_initial_ramp(self):
        self.run_case('stereo')

    def test_splitter_route_uses_the_initial_ramp(self):
        self.run_case('splitter')

    def test_late_buffer_still_gets_its_first_ramp(self):
        self.run_case('late')

    def test_adpcm_uses_the_same_voice_gain_ramp(self):
        self.run_case('adpcm')

    def test_recorded_rates_and_pitches_preserve_resampling(self):
        for mode in ('pitch32', 'pitch32slow'):
            with self.subTest(mode=mode):
                self.run_case(mode)

    def test_looping_does_not_restart_the_gain_ramp(self):
        self.run_case('loop')


if __name__ == '__main__':
    unittest.main()
