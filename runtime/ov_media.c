// Adapted from MacNativeGames games/totk/runtime/opus_codec.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// The hardware Opus decoder (nn::codec), decoded with libopus on the host. The software keyboard (nn::swkbd) is
// in swkbd_win.c.
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <opus/opus.h>

#include "host.h"
#include "platform/platform.h"

#define CODEC_R_INVALID 0x1fcd1u   // MacNativeGames' "invalid argument" result for nn::codec
#define OPUS_WORK_SIZE 0x9000u     // what the game is told to reserve: libopus stereo decoder state (about 26 KiB) plus 4 KiB
#define OPUS_MGR_WORK_SIZE 0x10000u
#define OPUS_MAX_FRAME_SAMPLES 5760 // 120 ms at 48 kHz

typedef struct OpusManager {
    uint64_t guest;
    uint64_t event;
    int max_rate, max_channels, max_decoders, max_io_buffers;
    int64_t max_frame_ns;
    int active_decoders;
    int active_io;
    struct OpusManager *next;
} OpusManager;

typedef struct OpusFrame {
    int16_t *pcm;
    int samples;
    int channels;
    int ready;   // 0 = waiting for Execute, 1 = completed & ready for Dequeue
    struct OpusFrame *next;
} OpusFrame;

// One record per initialised decoder object (the guest's HardwareOpusDecoderType). The registry lock only guards the
// list; a decoder's own lock serialises its libopus state, so different streams decode in parallel. Lock order:
// registry, then decoder (a caller takes the decoder lock before it drops the registry lock, so Finalize cannot free a
// decoder another thread is using).
typedef struct Decoder {
    uint64_t guest;
    OpusDecoder *dec;
    int rate, channels;
    OpusManager *mgr;
    OpusFrame *qhead, *qtail;
    int qwaiting, qcompleted;
    PlatMutex use;
    struct Decoder *next;
} Decoder;
static Decoder *decoders;
static OpusManager *managers;
static PlatMutex lock = PLAT_MUTEX_INIT;

static int valid_format(int rate, int channels) {
    return (rate == 48000 || rate == 24000 || rate == 16000 || rate == 12000 || rate == 8000) &&
           (channels == 1 || channels == 2);
}

// Returns the decoder with its use lock held.
static Decoder *acquire(uint64_t guest, const char *fn) {
    plat_mutex_lock_shared(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) plat_mutex_lock(&d->use);
    plat_mutex_unlock_shared(&lock);
    if (!d) rt_fatal("nn::codec::%s: unknown hardware Opus decoder %#llx", fn, (unsigned long long)guest);
    return d;
}

static void release(Decoder *d) { plat_mutex_unlock(&d->use); }

static OpusDecoder *opus_new(int rate, int channels) {
    int err = 0;
    OpusDecoder *o = opus_decoder_create(rate, channels, &err);
    if (!o || err != OPUS_OK) rt_fatal("nn::codec: opus_decoder_create(%d, %d): %s", rate, channels, opus_strerror(err));
    return o;
}

// size_t GetHardwareOpusDecoderWorkBufferSize(int rate, int channels, int option)
static void ov_work_size(Cpu *c) {
    ret_x(c, valid_format((int)arg_x(c, 0), (int)arg_x(c, 1)) ? OPUS_WORK_SIZE : 0);
}

// Result InitializeHardwareOpusDecoder(HardwareOpusDecoderType *d, int rate, int channels, void *work, size_t size,
// int option). The state lives in libopus on the host; the guest's work buffer is reserved but unused. Initialising
// the same object again without Finalize replaces its decoder.
static void ov_initialize(Cpu *c) {
    uint64_t guest = arg_x(c, 0), size = arg_x(c, 4);
    int rate = (int)arg_x(c, 1), channels = (int)arg_x(c, 2);
    if (!guest || !valid_format(rate, channels) || size < OPUS_WORK_SIZE) { ret_x(c, CODEC_R_INVALID); return; }
    OpusDecoder *fresh = opus_new(rate, channels);
    plat_mutex_lock(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) {
        plat_mutex_lock(&d->use);   // waits for a decode in flight
        OpusDecoder *old = d->dec;
        d->dec = fresh;
        d->rate = rate;
        d->channels = channels;
        plat_mutex_unlock(&d->use);
        plat_mutex_unlock(&lock);
        opus_decoder_destroy(old);
    } else {
        d = calloc(1, sizeof *d);
        if (!d) rt_fatal("nn::codec: decoder allocation");
        plat_mutex_init(&d->use);
        d->guest = guest;
        d->dec = fresh;
        d->rate = rate;
        d->channels = channels;
        d->next = decoders;
        decoders = d;
        plat_mutex_unlock(&lock);
    }
    ret_x(c, 0);
}

static void drain_decoder_queue_locked(Decoder *d, int *dropped_out) {
    int dropped = 0;
    OpusFrame *f = d->qhead;
    while (f) {
        OpusFrame *nx = f->next;
        free(f->pcm);
        free(f);
        dropped++;
        f = nx;
    }
    d->qhead = d->qtail = NULL;
    d->qwaiting = 0;
    d->qcompleted = 0;
    if (dropped_out) *dropped_out = dropped;
}

// void FinalizeHardwareOpusDecoder(HardwareOpusDecoderType *d)
static void ov_finalize(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    plat_mutex_lock(&lock);
    Decoder **p = &decoders;
    while (*p && (*p)->guest != guest) p = &(*p)->next;
    if (!*p) {
        plat_mutex_unlock(&lock);
        return;
    }
    Decoder *d = *p;
    plat_mutex_lock(&d->use);   // a decode in flight finishes first; the registry lock keeps new ones out
    *p = d->next;
    int dropped = 0;
    drain_decoder_queue_locked(d, &dropped);
    if (d->mgr) {
        if (d->mgr->active_decoders > 0) d->mgr->active_decoders--;
        d->mgr->active_io -= dropped;
        if (d->mgr->active_io < 0) d->mgr->active_io = 0;
    }
    plat_mutex_unlock(&lock);
    opus_decoder_destroy(d->dec);
    free(d);
}

// int GetHardwareOpusDecoderSampleRate / GetHardwareOpusDecoderChannelCount(const HardwareOpusDecoderType *d)
static void ov_sample_rate(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderSampleRate");
    int v = d->rate;
    release(d);
    ret_x(c, (uint64_t)v);
}

static void ov_channel_count(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderChannelCount");
    int v = d->channels;
    release(d);
    ret_x(c, (uint64_t)v);
}

// Result ResetHardwareOpusDecoder(HardwareOpusDecoderType *d)
static void ov_reset(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "ResetHardwareOpusDecoder");
    int dropped = 0;
    drain_decoder_queue_locked(d, &dropped);
    int ok = opus_decoder_ctl(d->dec, OPUS_RESET_STATE);
    OpusManager *mgr = d->mgr;
    release(d);
    if (mgr && dropped) {
        plat_mutex_lock(&lock);
        mgr->active_io -= dropped;
        if (mgr->active_io < 0) mgr->active_io = 0;
        plat_mutex_unlock(&lock);
    }
    ret_x(c, ok == OPUS_OK ? 0 : CODEC_R_INVALID);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Result DecodeOpusInterleavedWithHardware(HardwareOpusDecoderType *d, size_t *consumed, int *samples, int16_t *out,
// size_t out_size, const void *in, size_t in_size). The input is Nintendo's frame: an 8-byte big-endian header
// {payload size, final range} and the Opus packet. A header range of 0 is not a recorded value (some encoders write 0
// into every packet), so it is not checked. A range mismatch is logged once and the samples are kept.
static void ov_decode(Cpu *c) {
    static int32_t warned;
    uint64_t guest = arg_x(c, 0), out_size = arg_x(c, 4), in_size = arg_x(c, 6);
    size_t *consumed = arg_p(c, 1);
    int *samples = arg_p(c, 2);
    int16_t *out = arg_p(c, 3);
    const uint8_t *in = arg_p(c, 5);
    if (!consumed || !samples || !out || !in || in_size < 8) { ret_x(c, CODEC_R_INVALID); return; }
    uint32_t n = be32(in), range = be32(in + 4);
    if (n > in_size - 8 || n > INT_MAX) { ret_x(c, CODEC_R_INVALID); return; }
    Decoder *d = acquire(guest, "DecodeOpusInterleavedWithHardware");
    size_t capacity = out_size / (sizeof(int16_t) * (size_t)d->channels);
    if (capacity > INT_MAX) { release(d); ret_x(c, CODEC_R_INVALID); return; }
    int got = opus_decode(d->dec, in + 8, (opus_int32)n, out, (int)capacity, 0);
    opus_uint32 final = 0;
    if (got >= 0) opus_decoder_ctl(d->dec, OPUS_GET_FINAL_RANGE(&final));
    release(d);
    if (got < 0) {
        rt_log("nn::codec: opus_decode: %s", opus_strerror(got));
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    if (n && range && final != range && !plat_xchg32(&warned, 1))
        rt_log("nn::codec: Opus final range %#x != stream %#x (logged once)", (unsigned)final, (unsigned)range);
    *consumed = 8 + n;
    *samples = got;
    ret_x(c, 0);
}

// ---- Async batch HardwareOpusDecoderManager (nnSdk 18+/21+, used by Wwise 2024+)
static OpusManager *find_mgr_locked(uint64_t guest) {
    OpusManager *m = managers;
    while (m && m->guest != guest) m = m->next;
    return m;
}

// size_t GetHardwareOpusDecoderManagerWorkBufferSize(int sampleRate, int channelCount, TimeSpan frameDur, int decCount, int ioCount)
static void ov_mgr_work_size(Cpu *c) {
    int rate = (int)arg_x(c, 0), ch = (int)arg_x(c, 1);
    int dec_cnt = (int)arg_x(c, 3), io_cnt = (int)arg_x(c, 4);
    if (!valid_format(rate, ch) || dec_cnt <= 0 || io_cnt <= 0) { ret_x(c, 0); return; }
    ret_x(c, OPUS_MGR_WORK_SIZE);
}

// Result InitializeHardwareOpusDecoderManager(mgr, event, sampleRate, ch, frameDur, decCount, ioCount, work, [sp+0]=workSize)
static void ov_mgr_init(Cpu *c) {
    uint64_t guest = arg_x(c, 0), ev = arg_x(c, 1);
    int rate = (int)arg_x(c, 2), ch = (int)arg_x(c, 3);
    int64_t frame_ns = (int64_t)arg_x(c, 4);
    int dec_cnt = (int)arg_x(c, 5), io_cnt = (int)arg_x(c, 6);
    uint64_t work_size = *(const uint64_t *)(uintptr_t)c->sp;
    if (!guest || !valid_format(rate, ch) || dec_cnt <= 0 || io_cnt <= 0 || work_size < OPUS_MGR_WORK_SIZE) {
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    if (ev) kern_bind_system_event(c, ev, 1);
    plat_mutex_lock(&lock);
    OpusManager *m = find_mgr_locked(guest);
    if (!m) {
        m = calloc(1, sizeof *m);
        if (!m) rt_fatal("nn::codec: OpusManager allocation");
        m->guest = guest;
        m->next = managers;
        managers = m;
    }
    m->event = ev;
    m->max_rate = rate;
    m->max_channels = ch;
    m->max_frame_ns = frame_ns;
    m->max_decoders = dec_cnt;
    m->max_io_buffers = io_cnt;
    m->active_decoders = 0;
    m->active_io = 0;
    plat_mutex_unlock(&lock);
    rt_log("nn::codec: HardwareOpusDecoderManager initialized (%d Hz, %d ch, %d decoders, %d io buffers)",
           rate, ch, dec_cnt, io_cnt);
    ret_x(c, 0);
}

// void FinalizeHardwareOpusDecoderManager(mgr)
static void ov_mgr_fini(Cpu *c) {
    uint64_t guest = arg_x(c, 0);
    plat_mutex_lock(&lock);
    OpusManager **p = &managers;
    while (*p && (*p)->guest != guest) p = &(*p)->next;
    if (*p) {
        OpusManager *m = *p;
        *p = m->next;
        for (Decoder *d = decoders; d; d = d->next)
            if (d->mgr == m) d->mgr = NULL;
        free(m);
    }
    plat_mutex_unlock(&lock);
}

// Result InitializeHardwareOpusDecoder(dec, sampleRate, ch, maxFrameDur, mgr)
static void ov_init_with_mgr(Cpu *c) {
    uint64_t guest = arg_x(c, 0), mgr_guest = arg_x(c, 4);
    int rate = (int)arg_x(c, 1), ch = (int)arg_x(c, 2);
    if (!guest || !mgr_guest || !valid_format(rate, ch)) { ret_x(c, CODEC_R_INVALID); return; }
    OpusDecoder *fresh = opus_new(rate, ch);
    plat_mutex_lock(&lock);
    OpusManager *m = find_mgr_locked(mgr_guest);
    if (!m) {
        plat_mutex_unlock(&lock);
        opus_decoder_destroy(fresh);
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) {
        plat_mutex_lock(&d->use);
        OpusDecoder *old = d->dec;
        int dropped = 0;
        drain_decoder_queue_locked(d, &dropped);
        if (d->mgr && d->mgr != m) {
            if (d->mgr->active_decoders > 0) d->mgr->active_decoders--;
            d->mgr->active_io -= dropped;
            if (d->mgr->active_io < 0) d->mgr->active_io = 0;
            m->active_decoders++;
        } else if (d->mgr == m) {
            m->active_io -= dropped;
            if (m->active_io < 0) m->active_io = 0;
        } else {
            m->active_decoders++;
        }
        d->dec = fresh;
        d->rate = rate;
        d->channels = ch;
        d->mgr = m;
        plat_mutex_unlock(&d->use);
        plat_mutex_unlock(&lock);
        opus_decoder_destroy(old);
    } else {
        d = calloc(1, sizeof *d);
        if (!d) rt_fatal("nn::codec: decoder allocation");
        plat_mutex_init(&d->use);
        d->guest = guest;
        d->dec = fresh;
        d->rate = rate;
        d->channels = ch;
        d->mgr = m;
        d->next = decoders;
        decoders = d;
        m->active_decoders++;
        plat_mutex_unlock(&lock);
    }
    ret_x(c, 0);
}

// Result EnqueueOpusDataToHardwareOpusDecoder(dec, size_t *outConsumed, const void *in, size_t inSize)
static void ov_enqueue(Cpu *c) {
    static int32_t warned_hdr;
    uint64_t guest = arg_x(c, 0), in_size = arg_x(c, 3);
    size_t *consumed = arg_p(c, 1);
    const uint8_t *in = arg_p(c, 2);
    if (!consumed || !in || in_size < 8) { ret_x(c, CODEC_R_INVALID); return; }
    uint32_t n = be32(in);
    const uint8_t *payload = in + 8;
    size_t total_consumed = 8 + (size_t)n;
    if (n > in_size - 8 || n > INT_MAX) {
        if (!plat_xchg32(&warned_hdr, 1))
            rt_log("nn::codec: EnqueueOpusData bad header n=%u in_size=%llu", n, (unsigned long long)in_size);
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    Decoder *d = acquire(guest, "EnqueueOpusDataToHardwareOpusDecoder");
    int ch = d->channels;
    int16_t *pcm = malloc((size_t)OPUS_MAX_FRAME_SAMPLES * (size_t)ch * sizeof(int16_t));
    if (!pcm) rt_fatal("nn::codec: PCM buffer allocation");
    int got = opus_decode(d->dec, payload, (opus_int32)n, pcm, OPUS_MAX_FRAME_SAMPLES, 0);
    if (got < 0) {
        release(d);
        free(pcm);
        rt_log("nn::codec: EnqueueOpusData opus_decode: %s", opus_strerror(got));
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    OpusFrame *f = calloc(1, sizeof *f);
    if (!f) rt_fatal("nn::codec: OpusFrame allocation");
    f->pcm = pcm;
    f->samples = got;
    f->channels = ch;
    f->ready = 0;
    if (d->qtail) d->qtail->next = f;
    else d->qhead = f;
    d->qtail = f;
    d->qwaiting++;
    OpusManager *mgr = d->mgr;
    release(d);
    if (mgr) {
        plat_mutex_lock(&lock);
        mgr->active_io++;
        plat_mutex_unlock(&lock);
    }
    *consumed = total_consumed;
    ret_x(c, 0);
}

// Result ExecuteHardwareOpusDecoderDecoding(mgr)
static void ov_execute(Cpu *c) {
    uint64_t mgr_guest = arg_x(c, 0);
    uint64_t ev = 0;
    plat_mutex_lock(&lock);
    OpusManager *m = find_mgr_locked(mgr_guest);
    if (!m) { plat_mutex_unlock(&lock); ret_x(c, CODEC_R_INVALID); return; }
    ev = m->event;
    for (Decoder *d = decoders; d; d = d->next) {
        if (d->mgr != m) continue;
        plat_mutex_lock(&d->use);
        for (OpusFrame *f = d->qhead; f; f = f->next) {
            if (!f->ready) {
                f->ready = 1;
                d->qcompleted++;
            }
        }
        d->qwaiting = 0;
        plat_mutex_unlock(&d->use);
    }
    plat_mutex_unlock(&lock);
    if (ev) kern_signal_system_event(ev);
    ret_x(c, 0);
}

// Result DequeuePcmInterleavedDataFromHardwareOpusDecoder(dec, int *outSamples, int16_t *outBuf, size_t outSize)
static void ov_dequeue(Cpu *c) {
    uint64_t guest = arg_x(c, 0), out_size = arg_x(c, 3);
    int *samples = arg_p(c, 1);
    int16_t *out = arg_p(c, 2);
    if (!samples || !out) { ret_x(c, CODEC_R_INVALID); return; }
    Decoder *d = acquire(guest, "DequeuePcmInterleavedDataFromHardwareOpusDecoder");
    OpusFrame *f = d->qhead;
    if (!f || !f->ready) {
        release(d);
        *samples = 0;
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    size_t need = (size_t)f->samples * (size_t)f->channels * sizeof(int16_t);
    if (need > out_size) {
        release(d);
        *samples = 0;
        ret_x(c, CODEC_R_INVALID);
        return;
    }
    d->qhead = f->next;
    if (!d->qhead) d->qtail = NULL;
    if (d->qcompleted > 0) d->qcompleted--;
    OpusManager *mgr = d->mgr;
    release(d);
    if (mgr) {
        plat_mutex_lock(&lock);
        if (mgr->active_io > 0) mgr->active_io--;
        plat_mutex_unlock(&lock);
    }
    memcpy(out, f->pcm, need);
    *samples = f->samples;
    free(f->pcm);
    free(f);
    ret_x(c, 0);
}

static void ov_mgr_free_io(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    OpusManager *m = find_mgr_locked(arg_x(c, 0));
    int v = m ? (m->max_io_buffers - m->active_io) : 0;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, (uint64_t)(v > 0 ? v : 0));
}
static void ov_mgr_total_io(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    OpusManager *m = find_mgr_locked(arg_x(c, 0));
    int v = m ? m->max_io_buffers : 0;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, (uint64_t)v);
}
static void ov_mgr_using_io(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    OpusManager *m = find_mgr_locked(arg_x(c, 0));
    int v = m ? m->active_io : 0;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, (uint64_t)v);
}
static void ov_mgr_total_dec(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    OpusManager *m = find_mgr_locked(arg_x(c, 0));
    int v = m ? m->max_decoders : 0;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, (uint64_t)v);
}
static void ov_mgr_using_dec(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    OpusManager *m = find_mgr_locked(arg_x(c, 0));
    int v = m ? m->active_decoders : 0;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, (uint64_t)v);
}
static void ov_mgr_free_dec(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    OpusManager *m = find_mgr_locked(arg_x(c, 0));
    int v = m ? (m->max_decoders - m->active_decoders) : 0;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, (uint64_t)(v > 0 ? v : 0));
}
static void ov_mgr_validate(Cpu *c) { ret_x(c, 1); }
static void ov_dec_get_mgr(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderManager");
    uint64_t g = d->mgr ? d->mgr->guest : 0;
    release(d);
    ret_x(c, g);
}
static void ov_dec_waiting(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderWaitingDecodeRequestCount");
    int v = d->qwaiting;
    release(d);
    ret_x(c, (uint64_t)v);
}
static void ov_dec_executing(Cpu *c) { (void)c; ret_x(c, 0); }
static void ov_dec_completed(Cpu *c) {
    Decoder *d = acquire(arg_x(c, 0), "GetHardwareOpusDecoderCompletedDecodeRequestCount");
    int v = d->qcompleted;
    release(d);
    ret_x(c, (uint64_t)v);
}

HOST_OVERRIDE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", ov_work_size);
HOST_OVERRIDE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", ov_initialize);
HOST_OVERRIDE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiNS_8TimeSpanEPNS0_30HardwareOpusDecoderManagerTypeE", ov_init_with_mgr);
HOST_OVERRIDE("_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", ov_finalize);
HOST_OVERRIDE("_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", ov_sample_rate);
HOST_OVERRIDE("_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", ov_channel_count);
HOST_OVERRIDE("_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", ov_reset);
HOST_OVERRIDE("_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", ov_decode);
HOST_OVERRIDE("_ZN2nn5codec43GetHardwareOpusDecoderManagerWorkBufferSizeEiiNS_8TimeSpanEii", ov_mgr_work_size);
HOST_OVERRIDE("_ZN2nn5codec36InitializeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeEPNS_2os15SystemEventTypeEiiNS_8TimeSpanEiiPvm", ov_mgr_init);
HOST_OVERRIDE("_ZN2nn5codec34FinalizeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_fini);
HOST_OVERRIDE("_ZN2nn5codec36EnqueueOpusDataToHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPmPKvm", ov_enqueue);
HOST_OVERRIDE("_ZN2nn5codec34ExecuteHardwareOpusDecoderDecodingEPNS0_30HardwareOpusDecoderManagerTypeE", ov_execute);
HOST_OVERRIDE("_ZN2nn5codec48DequeuePcmInterleavedDataFromHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPiPsm", ov_dequeue);
HOST_OVERRIDE("_ZN2nn5codec46GetHardwareOpusDecoderManagerFreeIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_free_io);
HOST_OVERRIDE("_ZN2nn5codec47GetHardwareOpusDecoderManagerTotalIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_total_io);
HOST_OVERRIDE("_ZN2nn5codec47GetHardwareOpusDecoderManagerUsingIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_using_io);
HOST_OVERRIDE("_ZN2nn5codec54GetHardwareOpusDecoderManagerTotalDecoderResourceCountEPKNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_total_dec);
HOST_OVERRIDE("_ZN2nn5codec54GetHardwareOpusDecoderManagerUsingDecoderResourceCountEPKNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_using_dec);
HOST_OVERRIDE("_ZN2nn5codec53GetHardwareOpusDecoderManagerFreeDecoderResourceCountEPKNS0_30HardwareOpusDecoderManagerTypeE", ov_mgr_free_dec);
HOST_OVERRIDE("_ZN2nn5codec49ValidateHardwareOpusDecoderManagerDecoderResourceEPKNS0_30HardwareOpusDecoderManagerTypeEiiNS_8TimeSpanE", ov_mgr_validate);
HOST_OVERRIDE("_ZN2nn5codec29GetHardwareOpusDecoderManagerEPNS0_23HardwareOpusDecoderTypeE", ov_dec_get_mgr);
HOST_OVERRIDE("_ZN2nn5codec47GetHardwareOpusDecoderWaitingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", ov_dec_waiting);
HOST_OVERRIDE("_ZN2nn5codec49GetHardwareOpusDecoderExecutingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", ov_dec_executing);
HOST_OVERRIDE("_ZN2nn5codec49GetHardwareOpusDecoderCompletedDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", ov_dec_completed);
