// Horizon / Switch 2 audio output service (audout:u and aud:u cmd 1 IAudioOutManager / IAudioOut)
// backed by SDL3 audio stream playback and real-time buffer release pacing.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "audren.h"
#include "cfg.h"
#include "host.h"
#include "ipc.h"
#include "platform/platform.h"

#define AUDOUT_MAX_QUEUED 32u
#define AUDOUT_MAX_RELEASED 64u

typedef struct {
    uint64_t tag;
    uint64_t data_addr;
    uint64_t data_size;
} AudoutBuf;

typedef struct {
    PlatMutex lock;
    PlatThread *thread;
    volatile int32_t alive;
    uint32_t sample_rate, channels, format;
    volatile uint32_t state;   // 0 = Started, 1 = Stopped
    float volume;
    uint32_t event;
    uint64_t played_samples;
    AudoutBuf queued[AUDOUT_MAX_QUEUED];
    uint32_t q_head, q_count;
    uint64_t released[AUDOUT_MAX_RELEASED];
    uint32_t r_head, r_count;
    SDL_AudioStream *stream;
} AudioOut;

static IpcBuffer pick_buf(const IpcBuffer *a, unsigned na, const IpcBuffer *b, unsigned nb, unsigned i) {
    IpcBuffer x = i < na ? a[i] : (IpcBuffer){0}, y = i < nb ? b[i] : (IpcBuffer){0};
    return x.size ? x : y;
}

static void mark_auto_out(const IpcRequest *rq, IpcResponse *rs) {
    for (unsigned i = 0; i < rq->nc && rs->nxout < IPC_MAX_BUF; i++) {
        if (rq->c[i].size && (i >= rq->nb || !rq->b[i].size))
            rs->xout[rs->nxout++] = (IpcBuffer){rq->c[i].addr, rq->c[i].size, i};
    }
}

static void push_released_locked(AudioOut *o, uint64_t tag) {
    if (o->r_count >= AUDOUT_MAX_RELEASED) {
        o->r_head = (o->r_head + 1) % AUDOUT_MAX_RELEASED;
        o->r_count--;
    }
    o->released[(o->r_head + o->r_count) % AUDOUT_MAX_RELEASED] = tag;
    o->r_count++;
}

static int drain_queued_to_released_locked(AudioOut *o) {
    int moved = 0;
    while (o->q_count > 0) {
        AudoutBuf b = o->queued[o->q_head];
        o->q_head = (o->q_head + 1) % AUDOUT_MAX_QUEUED;
        o->q_count--;
        push_released_locked(o, b.tag);
        moved++;
    }
    return moved;
}

static void open_sdl_stream(AudioOut *o) {
    const char *mode = cfg_get("TOTK_AUDIO");
    if ((mode && !strcmp(mode, "none")) || cfg_get("TOTK_AUDIO_OFF")) {
        rt_log("audout: TOTK_AUDIO=none: running without audio device");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log("audout: SDL audio init failed (%s): running silently", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)o->channels, (int)o->sample_rate};
    o->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!o->stream) {
        rt_log("audout: no output device (%s): running silently", SDL_GetError());
        return;
    }
    SDL_SetAudioStreamGain(o->stream, o->volume);
}

static void audout_thread(void *arg) {
    AudioOut *o = arg;
    while (__atomic_load_n(&o->alive, __ATOMIC_ACQUIRE)) {
        AudoutBuf b = {0};
        int have = 0;
        uint32_t rate = 48000, ch = 2;
        SDL_AudioStream *stream = NULL;

        plat_mutex_lock(&o->lock);
        if (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 0 && o->q_count > 0) {
            b = o->queued[o->q_head];
            o->q_head = (o->q_head + 1) % AUDOUT_MAX_QUEUED;
            o->q_count--;
            rate = o->sample_rate;
            ch = o->channels;
            stream = o->stream;
            have = 1;
        }
        plat_mutex_unlock(&o->lock);

        if (!have) {
            plat_sleep_ms(2);
            continue;
        }

        if (stream && b.data_size > 0 && b.data_size <= (16ull << 20) &&
            plat_mem_readable((uintptr_t)b.data_addr, (size_t)b.data_size)) {
            SDL_PutAudioStreamData(stream, (const void *)(uintptr_t)b.data_addr, (int)b.data_size);
        }

        uint64_t bpf = (uint64_t)ch * 2u;
        uint64_t frames = bpf ? b.data_size / bpf : 0;
        if (stream) {
            uint64_t limit = b.data_size * 2u;
            while (__atomic_load_n(&o->alive, __ATOMIC_ACQUIRE) &&
                   __atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 0 &&
                   (uint64_t)SDL_GetAudioStreamQueued(stream) > limit) {
                plat_sleep_ms(2);
            }
        } else {
            uint64_t dur_ms = (frames && rate) ? (frames * 1000ull + rate - 1) / rate : 1;
            if (dur_ms > 200) dur_ms = 200;
            for (uint64_t elapsed = 0;
                 elapsed < dur_ms &&
                 __atomic_load_n(&o->alive, __ATOMIC_ACQUIRE) &&
                 __atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 0;
                 elapsed += 5) {
                uint64_t step = dur_ms - elapsed;
                plat_sleep_ms((uint32_t)(step > 5 ? 5 : step));
            }
        }

        plat_mutex_lock(&o->lock);
        o->played_samples += frames;
        push_released_locked(o, b.tag);
        plat_mutex_unlock(&o->lock);
        kern_event_signal(o->event);
    }
}

static void audout_close(void *arg) {
    AudioOut *o = arg;
    if (!o) return;
    plat_xchg32(&o->alive, 0);
    int away = kern_core_away();
    if (o->thread) plat_thread_join(o->thread);
    kern_core_back(&away);
    if (o->stream) SDL_DestroyAudioStream(o->stream);
    if (o->event) kern_handle_close(o->event);
    free(o);
}

static void audout_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    switch (rq->cmd) {
    case 0: {   // GetAudioOutState
        uint32_t st = __atomic_load_n(&o->state, __ATOMIC_ACQUIRE);
        IPC_OUT(rs, st);
        return;
    }
    case 1:   // StartAudioOut
        plat_mutex_lock(&o->lock);
        __atomic_store_n(&o->state, 0u, __ATOMIC_RELEASE);
        if (o->stream) SDL_ResumeAudioStreamDevice(o->stream);
        plat_mutex_unlock(&o->lock);
        return;
    case 2: {   // StopAudioOut
        int moved;
        plat_mutex_lock(&o->lock);
        __atomic_store_n(&o->state, 1u, __ATOMIC_RELEASE);
        if (o->stream) {
            SDL_PauseAudioStreamDevice(o->stream);
            SDL_ClearAudioStream(o->stream);
        }
        moved = drain_queued_to_released_locked(o);
        plat_mutex_unlock(&o->lock);
        if (moved) kern_event_signal(o->event);
        return;
    }
    case 3: case 7: {   // AppendAudioOutBuffer / AppendAudioOutBufferAuto
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        IpcBuffer in = pick_buf(rq->a, rq->na, rq->x, rq->nx, 0);
        if (in.size < 40) { rs->result = 0x299u; return; }
        const uint64_t *d = ipc_guest(in.addr, 40);
        uint64_t data_addr = d[1] + d[4];
        uint64_t data_size = d[3];
        if (data_addr < d[1]) { rs->result = 0x299u; return; }
        plat_mutex_lock(&o->lock);
        if (o->q_count >= AUDOUT_MAX_QUEUED) {
            plat_mutex_unlock(&o->lock);
            rs->result = 0x299u;
            return;
        }
        o->queued[(o->q_head + o->q_count) % AUDOUT_MAX_QUEUED] = (AudoutBuf){tag, data_addr, data_size};
        o->q_count++;
        plat_mutex_unlock(&o->lock);
        return;
    }
    case 4:   // RegisterBufferEvent
        rs->copy[rs->ncopy++] = kern_event_readable(o->event);
        return;
    case 5: case 8: {   // GetReleasedAudioOutBuffers / GetReleasedAudioOutBuffersAuto
        IpcBuffer out = pick_buf(rq->b, rq->nb, rq->c, rq->nc, 0);
        uint64_t *dst = out.size >= 8 ? ipc_guest(out.addr, out.size) : NULL;
        uint32_t max_tags = dst ? (uint32_t)(out.size / 8) : 0;
        uint32_t count = 0;
        plat_mutex_lock(&o->lock);
        while (count < max_tags && o->r_count > 0) {
            dst[count++] = o->released[o->r_head];
            o->r_head = (o->r_head + 1) % AUDOUT_MAX_RELEASED;
            o->r_count--;
        }
        plat_mutex_unlock(&o->lock);
        if (rq->cmd == 8) mark_auto_out(rq, rs);
        IPC_OUT(rs, count);
        return;
    }
    case 6: {   // ContainsAudioOutBuffer
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        uint8_t found = 0;
        plat_mutex_lock(&o->lock);
        for (uint32_t i = 0; i < o->q_count; i++) {
            if (o->queued[(o->q_head + i) % AUDOUT_MAX_QUEUED].tag == tag) {
                found = 1;
                break;
            }
        }
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, found);
        return;
    }
    case 9: {   // GetAudioOutBufferCount
        plat_mutex_lock(&o->lock);
        uint32_t cnt = o->q_count;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, cnt);
        return;
    }
    case 10: {   // GetAudioOutPlayedSampleCount
        plat_mutex_lock(&o->lock);
        uint64_t played = o->played_samples;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, played);
        return;
    }
    case 11: {   // FlushAudioOutBuffers
        int moved;
        plat_mutex_lock(&o->lock);
        if (o->stream) SDL_ClearAudioStream(o->stream);
        moved = drain_queued_to_released_locked(o);
        plat_mutex_unlock(&o->lock);
        if (moved) kern_event_signal(o->event);
        uint8_t flushed = moved > 0 ? 1 : 0;
        IPC_OUT(rs, flushed);
        return;
    }
    case 12: {   // SetAudioOutVolume
        float vol = IPC_ARG(rq, float, 0);
        plat_mutex_lock(&o->lock);
        o->volume = vol;
        if (o->stream) SDL_SetAudioStreamGain(o->stream, vol);
        plat_mutex_unlock(&o->lock);
        return;
    }
    case 13: {   // GetAudioOutVolume
        plat_mutex_lock(&o->lock);
        float vol = o->volume;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, vol);
        return;
    }
    default:
        ipc_unimplemented(s, rq);
    }
}

void audout_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    static const char dev_name[] = "DeviceOut";
    switch (rq->cmd) {
    case 0: case 2: {   // ListAudioOuts / ListAudioOutsAuto
        IpcBuffer out = pick_buf(rq->b, rq->nb, rq->c, rq->nc, 0);
        if (out.size > 0) {
            uint64_t n = out.size < 0x100 ? out.size : 0x100;
            uint8_t *dst = ipc_guest(out.addr, n);
            memset(dst, 0, (size_t)n);
            size_t cp = sizeof(dev_name) < (size_t)n ? sizeof(dev_name) : (size_t)n;
            memcpy(dst, dev_name, cp);
            dst[cp - 1] = '\0';
        }
        if (rq->cmd == 2) mark_auto_out(rq, rs);
        uint32_t count = 1;
        IPC_OUT(rs, count);
        return;
    }
    case 1: case 3: {   // OpenAudioOut / OpenAudioOutAuto
        uint32_t rate = rq->data_size >= 4 ? IPC_ARG(rq, uint32_t, 0) : 0;
        uint16_t ch = rq->data_size >= 6 ? IPC_ARG(rq, uint16_t, 4) : 0;
        rate = rate ? rate : 48000;
        ch = (ch >= 1 && ch <= 6) ? ch : 2;

        IpcBuffer out = pick_buf(rq->b, rq->nb, rq->c, rq->nc, 0);
        if (out.size > 0) {
            uint64_t n = out.size < 0x100 ? out.size : 0x100;
            uint8_t *dst = ipc_guest(out.addr, n);
            memset(dst, 0, (size_t)n);
            size_t cp = sizeof(dev_name) < (size_t)n ? sizeof(dev_name) : (size_t)n;
            memcpy(dst, dev_name, cp);
            dst[cp - 1] = '\0';
        }
        if (rq->cmd == 3) mark_auto_out(rq, rs);

        AudioOut *o = calloc(1, sizeof *o);
        if (!o) rt_fatal("audout: out of memory");
        o->lock = (PlatMutex)PLAT_MUTEX_INIT;
        o->alive = 1;
        o->sample_rate = rate;
        o->channels = ch;
        o->format = 2;
        o->state = 1;
        o->volume = 1.0f;
        o->event = kern_event_new();
        open_sdl_stream(o);
        o->thread = plat_thread_new(audout_thread, o, "audout");
        if (!o->thread) rt_fatal("audout: cannot start playback thread");

        rt_log("audout: opened 'DeviceOut' %u Hz %u ch PCM16", o->sample_rate, o->channels);
        struct { uint32_t rate, ch, fmt, state; } out_p = {o->sample_rate, o->channels, 2u, 1u};
        IPC_OUT(rs, out_p);
        rs->move[rs->nmove++] = ipc_session_new("IAudioOut", audout_handler, o, audout_close);
        return;
    }
    default:
        ipc_unimplemented(s, rq);
    }
}

// ---- Switch 2 nn::audio::OutputStream API (nnSdk 21+, used by Wwise 2024+) backed by SDL3
typedef struct OutputStreamNode {
    uint64_t guest;
    uint64_t event;
    uint32_t sample_format;   // 1=S8, 2=S16, 3=S24, 4=S32, 5=F32
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t bytes_per_frame;
    int buf_frames;
    int notify_frames;
    volatile int32_t state;   // 0=Closed, 1=Stopped, 2=Started
    volatile int32_t alive;
    float gain;
    SDL_AudioStream *sdl;
    PlatMutex lock;
    PlatThread *thread;
    struct OutputStreamNode *next;
} OutputStreamNode;

static OutputStreamNode *g_out_streams;
static PlatMutex g_out_lock = PLAT_MUTEX_INIT;

static OutputStreamNode *out_stream_find_locked(uint64_t guest) {
    for (OutputStreamNode *s = g_out_streams; s; s = s->next)
        if (s->guest == guest) return s;
    return NULL;
}

static int out_stream_free_frames_locked(OutputStreamNode *s) {
    if (!s || s->buf_frames <= 0 || s->bytes_per_frame == 0) return 0;
    int queued_frames = 0;
    if (s->sdl) {
        int qbytes = SDL_GetAudioStreamQueued(s->sdl);
        if (qbytes > 0) queued_frames = qbytes / (int)s->bytes_per_frame;
    }
    int free_f = s->buf_frames - queued_frames;
    return free_f > 0 ? free_f : 0;
}

static void out_stream_pacer(void *arg) {
    OutputStreamNode *s = arg;
    while (__atomic_load_n(&s->alive, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == 2 && s->event) {
            plat_mutex_lock(&s->lock);
            int free_f = out_stream_free_frames_locked(s);
            int thresh = s->notify_frames > 0 ? s->notify_frames : (s->buf_frames / 4);
            plat_mutex_unlock(&s->lock);
            if (free_f >= thresh) kern_signal_system_event(s->event);
        }
        plat_sleep_ms(4);
    }
}

static void out_stream_open_common(Cpu *c, uint64_t guest, uint64_t ev, const uint32_t *fmt,
                                   int buf_frames, int notify_frames) {
    if (!guest || !fmt || buf_frames <= 0) { ret_x(c, 0x10299u); return; }
    uint32_t sfmt = fmt[0], ch = fmt[1], rate = fmt[2];
    if (ch < 1 || ch > 8) ch = 2;
    if (rate < 8000 || rate > 192000) rate = 48000;
    SDL_AudioFormat sdl_fmt = SDL_AUDIO_F32;
    uint32_t bps = 4;
    if (sfmt == 2) { sdl_fmt = SDL_AUDIO_S16; bps = 2; }
    else if (sfmt == 4) { sdl_fmt = SDL_AUDIO_S32; bps = 4; }
    else if (sfmt == 1) { sdl_fmt = SDL_AUDIO_S8; bps = 1; }
    if (ev) kern_bind_system_event(c, ev, 1);

    OutputStreamNode *s = calloc(1, sizeof *s);
    if (!s) rt_fatal("audout: OutputStream out of memory");
    s->guest = guest;
    s->event = ev;
    s->sample_format = sfmt;
    s->channels = ch;
    s->sample_rate = rate;
    s->bytes_per_frame = ch * bps;
    s->buf_frames = buf_frames;
    s->notify_frames = notify_frames > 0 ? notify_frames : (buf_frames / 2);
    s->state = 1;
    s->alive = 1;
    s->gain = 1.0f;
    plat_mutex_init(&s->lock);

    const char *mode = cfg_get("TOTK_AUDIO");
    if (!((mode && !strcmp(mode, "none")) || cfg_get("TOTK_AUDIO_OFF"))) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_AudioSpec spec = {sdl_fmt, (int)ch, (int)rate};
            s->sdl = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (s->sdl) {
                SDL_SetAudioStreamGain(s->sdl, s->gain);
                SDL_PauseAudioStreamDevice(s->sdl);
            }
        }
    }
    if (ev) s->thread = plat_thread_new(out_stream_pacer, s, "audout_stream");

    plat_mutex_lock(&g_out_lock);
    s->next = g_out_streams;
    g_out_streams = s;
    plat_mutex_unlock(&g_out_lock);

    rt_log("audout: OutputStream opened %u Hz %u ch fmt=%u buf=%d frames (sdl=%s)",
           rate, ch, sfmt, buf_frames, s->sdl ? "ok" : "silent");
    ret_x(c, 0);
}

static void ov_open_output_stream_ev_notify(Cpu *c) {
    out_stream_open_common(c, arg_x(c, 0), arg_x(c, 1), arg_p(c, 5), (int)arg_x(c, 6), (int)arg_x(c, 7));
}
static void ov_open_output_stream_ev(Cpu *c) {
    out_stream_open_common(c, arg_x(c, 0), arg_x(c, 1), arg_p(c, 5), (int)arg_x(c, 6), (int)arg_x(c, 6) / 2);
}
static void ov_open_output_stream_noev(Cpu *c) {
    out_stream_open_common(c, arg_x(c, 0), 0, arg_p(c, 4), (int)arg_x(c, 5), 0);
}
static void ov_start_output_stream(Cpu *c) {
    plat_mutex_lock(&g_out_lock);
    OutputStreamNode *s = out_stream_find_locked(arg_x(c, 0));
    if (s) {
        plat_mutex_lock(&s->lock);
        __atomic_store_n(&s->state, 2, __ATOMIC_RELEASE);
        if (s->sdl) SDL_ResumeAudioStreamDevice(s->sdl);
        uint64_t ev = s->event;
        plat_mutex_unlock(&s->lock);
        plat_mutex_unlock(&g_out_lock);
        if (ev) kern_signal_system_event(ev);
        ret_x(c, 0);
        return;
    }
    plat_mutex_unlock(&g_out_lock);
    ret_x(c, 0);
}
static void ov_stop_output_stream(Cpu *c) {
    plat_mutex_lock(&g_out_lock);
    OutputStreamNode *s = out_stream_find_locked(arg_x(c, 0));
    if (s) {
        plat_mutex_lock(&s->lock);
        __atomic_store_n(&s->state, 1, __ATOMIC_RELEASE);
        if (s->sdl) SDL_PauseAudioStreamDevice(s->sdl);
        plat_mutex_unlock(&s->lock);
    }
    plat_mutex_unlock(&g_out_lock);
    ret_x(c, 0);
}
static void ov_purge_output_stream(Cpu *c) {
    plat_mutex_lock(&g_out_lock);
    OutputStreamNode *s = out_stream_find_locked(arg_x(c, 0));
    if (s) {
        plat_mutex_lock(&s->lock);
        if (s->sdl) SDL_ClearAudioStream(s->sdl);
        plat_mutex_unlock(&s->lock);
    }
    plat_mutex_unlock(&g_out_lock);
    ret_x(c, 0);
}
static void ov_close_output_stream(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    plat_mutex_lock(&g_out_lock);
    OutputStreamNode **pp = &g_out_streams;
    while (*pp && (*pp)->guest != guest) pp = &(*pp)->next;
    OutputStreamNode *s = *pp;
    if (s) *pp = s->next;
    plat_mutex_unlock(&g_out_lock);
    if (!s) return;
    __atomic_store_n(&s->state, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->alive, 0, __ATOMIC_RELEASE);
    if (s->thread) plat_thread_join(s->thread);
    if (s->sdl) SDL_DestroyAudioStream(s->sdl);
    free(s);
}
static void ov_write_output_stream(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    const void *buf = arg_p(c, 1);
    int frames = (int)arg_x(c, 2);
    if (!buf || frames <= 0) { ret_x(c, 0); return; }
    plat_mutex_lock(&g_out_lock);
    OutputStreamNode *s = out_stream_find_locked(guest);
    if (s) {
        plat_mutex_lock(&s->lock);
        int free_f = out_stream_free_frames_locked(s);
        int to_write = frames <= free_f ? frames : free_f;
        if (to_write <= 0 && !s->sdl) to_write = frames;
        if (s->sdl && to_write > 0)
            SDL_PutAudioStreamData(s->sdl, buf, to_write * (int)s->bytes_per_frame);
        plat_mutex_unlock(&s->lock);
        plat_mutex_unlock(&g_out_lock);
        ret_x(c, (uint64_t)to_write);
        return;
    }
    plat_mutex_unlock(&g_out_lock);
    ret_x(c, (uint64_t)frames);
}
static void ov_set_output_stream_gain(Cpu *c) {
    float gain = 1.0f;
    uint32_t bits = (uint32_t)c->v[0];
    memcpy(&gain, &bits, sizeof gain);
    plat_mutex_lock(&g_out_lock);
    OutputStreamNode *s = out_stream_find_locked(arg_x(c, 0));
    if (s) {
        plat_mutex_lock(&s->lock);
        s->gain = gain;
        if (s->sdl) SDL_SetAudioStreamGain(s->sdl, gain);
        plat_mutex_unlock(&s->lock);
    }
    plat_mutex_unlock(&g_out_lock);
}
static void ov_get_output_stream_state(Cpu *c) {
    plat_mutex_lock(&g_out_lock);
    OutputStreamNode *s = out_stream_find_locked(arg_x(c, 0));
    int st = s ? __atomic_load_n(&s->state, __ATOMIC_ACQUIRE) : 0;
    plat_mutex_unlock(&g_out_lock);
    ret_x(c, (uint64_t)st);
}
static void ov_get_output_stream_free_frames(Cpu *c) {
    plat_mutex_lock(&g_out_lock);
    OutputStreamNode *s = out_stream_find_locked(arg_x(c, 0));
    int free_f = 0;
    if (s) {
        plat_mutex_lock(&s->lock);
        free_f = out_stream_free_frames_locked(s);
        plat_mutex_unlock(&s->lock);
    }
    plat_mutex_unlock(&g_out_lock);
    ret_x(c, (uint64_t)free_f);
}
static void ov_get_output_stream_stats(Cpu *c) {
    void *dst = (void *)(uintptr_t)c->x[8];
    if (dst && plat_mem_readable(c->x[8], 32)) memset(dst, 0, 32);
    c->x[0] = 0;
    c->x[1] = 0;
}

HOST_OVERRIDE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE", ov_open_output_stream_ev_notify);
HOST_OVERRIDE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE", ov_open_output_stream_ev);
HOST_OVERRIDE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE", ov_open_output_stream_noev);
HOST_OVERRIDE("_ZN2nn5audio17StartOutputStreamEPNS0_16OutputStreamTypeE", ov_start_output_stream);
HOST_OVERRIDE("_ZN2nn5audio16StopOutputStreamEPNS0_16OutputStreamTypeE", ov_stop_output_stream);
HOST_OVERRIDE("_ZN2nn5audio17PurgeOutputStreamEPNS0_16OutputStreamTypeE", ov_purge_output_stream);
HOST_OVERRIDE("_ZN2nn5audio17CloseOutputStreamEPNS0_16OutputStreamTypeE", ov_close_output_stream);
HOST_OVERRIDE("_ZN2nn5audio17WriteOutputStreamEPNS0_16OutputStreamTypeEPKvi", ov_write_output_stream);
HOST_OVERRIDE("_ZN2nn5audio19SetOutputStreamGainEPNS0_16OutputStreamTypeEfNS_8TimeSpanE", ov_set_output_stream_gain);
HOST_OVERRIDE("_ZN2nn5audio20GetOutputStreamStateEPKNS0_16OutputStreamTypeE", ov_get_output_stream_state);
HOST_OVERRIDE("_ZN2nn5audio29GetOutputStreamFreeFrameCountEPKNS0_16OutputStreamTypeE", ov_get_output_stream_free_frames);
HOST_OVERRIDE("_ZN2nn5audio25GetOutputStreamStatisticsEPNS0_16OutputStreamTypeEb", ov_get_output_stream_stats);
