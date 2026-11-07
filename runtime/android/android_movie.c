/* android_movie.c - movie:: (the SDK's video player library) on Android NDK media (AMediaExtractor + AMediaCodec).
 *
 * Generated from nnmovie_mf.c (the Windows player on Media Foundation) by keeping its protocol code - the Extractor and
 * Decoder virtual functions, the decoder's input / output buffer lists and events, the sample tokens, all described in
 * nnmovie_mf.c's header - and replacing what Media Foundation did:
 *   - the file is read through the game's ClientStreamReader into memory (as on Windows) and given to an
 *     AMediaExtractor through an AMediaDataSource;
 *   - SelectTrack makes an AMediaCodec for the track (video asked for NV12, colour format 21);
 *   - the "next sample of a track" (ext_fill) is DECODED here: the extractor's interleaved samples are fed to the codec
 *     of their track and the codecs' outputs are queued per track, so one track can be read ahead of the other;
 *   - a decoded sample is a host block: tightly packed NV12 at the track's size (converted from the codec's layout:
 *     NV12 / Qualcomm NV12 with stride and slice height, or I420) or interleaved 16-bit PCM.
 * The game asks for these when it shows a play-guide clip (content:/MovieData/PlayGuideMovie*.mp4, H.264) or the
 * opening / ending movies; MovieDecoderPlayer does not check that a decoder exists, so without a player it crashed in
 * Cap Kingdom (Galaxy S24 Ultra, 2026-10-28).
 * Environment: SMO_MOVIE_LOG=1 logs every decoder call; SMO_MOVIE_SAVE=<file> saves what the game's reader returned. */
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaDataSource.h>
#include "runtime.h"
#include "hostrt.h"

void input_mark(const char *what);                       /* nnhid.c: anchor for scripted input */

#include "nnmovie_common.inc"

/* A decoded sample (Media Foundation's IMFSample on Windows): packed NV12 at the track's size, or 16-bit PCM */
typedef struct MSample { uint8_t *data; size_t size; } MSample;
#define IMFSample MSample
static void IMFSample_Release(MSample *s) { if (s) { free(s->data); free(s); } }

/* ---------------------------------------------------------------- Android NDK media plumbing */
typedef struct {
    int kind;                                         /* 0 video, 1 audio, 2 other */
    char mime[48];
    int w, h, rate, ch, selected, eos;
    double fps;
    uint32_t fourcc;
    int failed;                                       /* the codec reported an error for this track */
    /* Android: the codec's output layout (video) */
    int cfmt, stride, slice, cl, ct, codec_eos, in_eos;
} Track;

#define QMAX 256                                      /* decoded samples queued per track */
typedef struct Ext {
    plat_mutex cs;
    AMediaExtractor *rd;                              /* non-NULL once a source is open (the Windows code tests rd) */
    AMediaDataSource *ds;
    unsigned char *bytes; size_t nbytes;              /* the file, kept for the data source */
    int ntracks;
    Track tr[MAX_TRACKS];
    AMediaFormat *fmt[MAX_TRACKS];
    AMediaCodec *codec[MAX_TRACKS];
    struct { MSample *s; int64_t us; } q[MAX_TRACKS][QMAX];
    int qh[MAX_TRACKS], qn[MAX_TRACKS];
    int src_eos;
    int64_t duration_us;
    int have_pending, pend_track, pend_consumed, eos;
    IMFSample *pending;
    int64_t pend_us;
    struct { int have; IMFSample *s; int64_t us; } ahead[MAX_TRACKS];    /* next decoded sample of each selected track */
    char path[260];
    unsigned long long samples_read;
    volatile uint64_t gen;                            /* bumped by SeekTo: samples read before it are refused by the decoders */
} Ext;

/* Decoder buffer model, as in the real library [subsdk DecoderImpl 0x1e440 Flush, 0x1fa30 AcquireInputBufferIndex,
 * 0x1e710 AcquireOutputBufferIndex, 0x1ff40/0x20050 Add...List]: TWO independent pools.
 *   input buffers  0..nin-1 : the codec offers an index when it can take data ("input list", one event per index);
 *                             AcquireInputBufferIndex pops the first one (*more = how many are left, 0x1011 when the
 *                             list is empty); SendInputBufferForDecode gives it back to the codec, which offers it
 *                             again as soon as the data is consumed - it does NOT wait for the decoded frame to be shown.
 *   output buffers 0..nout-1: a decoded frame sits in one from "output list" (AcquireOutputBufferIndex pops, 0x100e
 *                             when empty) until ReleaseOutputBufferIndex. When every output buffer is in use the codec
 *                             stops consuming input (back-pressure): the sent input stays "in decode".
 * Flush [subsdk 0x1e440] = MediaCodec::flush + both lists emptied, no event: every index the client still holds is
 * void, and nothing is offered until Start (MediaCodec::start after a flush offers all input buffers again).
 * The game's Seek [game 0x71036e7620] does exactly Flush(video), Flush(audio), empties its own index lists,
 * Extractor::SeekTo, sleeps 16 ms, empties the lists again, Start(video), Start(audio). */
#define MAX_IN 16
#define MAX_OUT 64
enum { IN_CODEC = 0, IN_OFFERED, IN_CLIENT, IN_DECODING };
enum { OUT_FREE = 0, OUT_READY, OUT_CLIENT };
typedef struct { int state; IMFSample *s; int64_t ts; uint32_t flags; } OutBuf;

typedef struct {
    plat_mutex cs;
    int type, fmt, mode, is_video;
    Ext *ext;
    int track;
    uint64_t events;                                  /* guest DecoderEvents* */
    int started, flushed, fmt_signalled;
    int nin, nout;
    unsigned char in_state[MAX_IN];
    int in_list[MAX_IN], in_n;                        /* offered input indices, oldest first */
    struct { IMFSample *s; int64_t ts; uint32_t flags; } in_data[MAX_IN];   /* what an IN_DECODING buffer holds */
    int dec_list[MAX_IN], dec_n;                      /* inputs waiting for a free output buffer, oldest first */
    OutBuf out[MAX_OUT];
    int out_list[MAX_OUT], out_n;                     /* decoded frames ready for the client, oldest first */
    unsigned char *mem;                               /* nin * SLOT_BYTES: the "input buffers" */
    int w, h, rate, ch;
    unsigned long long in_count, out_count, copied, dropped, flushes;
    int warned_small, warned_token;
    uint32_t last_error;                              /* DecoderImpl +0xd8 [subsdk 0x1fc80] */
} Dec;

typedef struct { uint64_t magic; IMFSample *s; int64_t ts; uint64_t gen; } Token;   /* gen = Ext::gen when it was read */

static void signal_event(Dec *d, int which) {
    if (!d->events) return;
    uint64_t ev = rd64(d->events + (uint64_t)which * 8);
    if (ev) os_signal_event(ev);
}

static void ext_lock(Ext *e) { plat_mutex_lock(&e->cs); }
static void ext_unlock(Ext *e) { plat_mutex_unlock(&e->cs); }

static Ext *ext_of(uint64_t guest) { return guest ? (Ext *)(uintptr_t)rd64(guest + 8) : NULL; }
static Dec *dec_of(uint64_t guest) { return guest ? (Dec *)(uintptr_t)rd64(guest + 8) : NULL; }

static void release_pending(Ext *e) {                 /* the current sample is done with (Advance) */
    if (e->have_pending && !e->pend_consumed && e->pending) IMFSample_Release(e->pending);
    if (e->have_pending) { e->ahead[e->pend_track].have = 0; e->ahead[e->pend_track].s = NULL; }
    e->have_pending = 0; e->pending = NULL; e->pend_consumed = 0;
}
static void release_ahead(Ext *e, int track) {        /* track < 0: all */
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (track >= 0 && i != track) continue;
        if (e->have_pending && e->pend_track == i) continue;                  /* owned by release_pending */
        if (e->ahead[i].have && e->ahead[i].s) IMFSample_Release(e->ahead[i].s);
        e->ahead[i].have = 0; e->ahead[i].s = NULL;
    }
}

static void q_clear(Ext *e, int t) {
    while (e->qn[t]) { IMFSample_Release(e->q[t][e->qh[t]].s); e->qh[t] = (e->qh[t] + 1) % QMAX; e->qn[t]--; }
    e->qh[t] = 0;
}
static ssize_t ds_read(void *ud, off64_t off, void *buf, size_t n) {
    Ext *e = ud;
    if (off < 0 || (size_t)off >= e->nbytes) return -1;                    /* the end */
    if (n > e->nbytes - (size_t)off) n = e->nbytes - (size_t)off;
    memcpy(buf, e->bytes + off, n);
    return (ssize_t)n;
}
static ssize_t ds_size(void *ud) { return (ssize_t)((Ext *)ud)->nbytes; }
static void ds_close(void *ud) { (void)ud; }

static int ext_open_bytes(Ext *e, unsigned char *data, size_t size) {
    e->bytes = data; e->nbytes = size;
    e->ds = AMediaDataSource_new();
    if (!e->ds) { printf("movie: AMediaDataSource_new failed\n"); return 0; }
    AMediaDataSource_setUserdata(e->ds, e);
    AMediaDataSource_setReadAt(e->ds, ds_read);
    AMediaDataSource_setGetSize(e->ds, ds_size);
    AMediaDataSource_setClose(e->ds, ds_close);
    e->rd = AMediaExtractor_new();
    media_status_t st = e->rd ? AMediaExtractor_setDataSourceCustom(e->rd, e->ds) : AMEDIA_ERROR_UNKNOWN;
    if (st != AMEDIA_OK) {
        printf("movie: %s: the extractor cannot open it (media status %d)\n", e->path, (int)st);
        if (e->rd) AMediaExtractor_delete(e->rd);
        e->rd = NULL;
        return 0;
    }
    size_t n = AMediaExtractor_getTrackCount(e->rd);
    e->ntracks = 0;
    for (size_t i = 0; i < n && e->ntracks < MAX_TRACKS; i++) {
        AMediaFormat *f = AMediaExtractor_getTrackFormat(e->rd, i);
        Track *tr = &e->tr[e->ntracks];
        memset(tr, 0, sizeof *tr);
        e->fmt[e->ntracks++] = f;
        const char *mime = NULL;
        AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
        snprintf(tr->mime, sizeof tr->mime, "%s", mime ? mime : "application/octet-stream");
        int64_t d = 0;
        if (AMediaFormat_getInt64(f, AMEDIAFORMAT_KEY_DURATION, &d) && d > e->duration_us) e->duration_us = d;
        if (!strncmp(tr->mime, "video/", 6)) {
            int32_t v = 0;
            tr->kind = 0;
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &tr->w);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &tr->h);
            float fr = 0;
            if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, &v)) tr->fps = v;
            else if (AMediaFormat_getFloat(f, AMEDIAFORMAT_KEY_FRAME_RATE, &fr)) tr->fps = fr;
        } else if (!strncmp(tr->mime, "audio/", 6)) {
            tr->kind = 1;
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &tr->rate);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &tr->ch);
        } else tr->kind = 2;
    }
    printf("movie: opened %s: %zu bytes, %.1f s, %d tracks:", e->path, size, e->duration_us / 1e6, e->ntracks);
    for (int i = 0; i < e->ntracks; i++) {
        Track *tr = &e->tr[i];
        if (tr->kind == 0) printf("  [%d] %s %dx%d %.2f fps", i, tr->mime, tr->w, tr->h, tr->fps);
        else if (tr->kind == 1) printf("  [%d] %s %d Hz %d ch", i, tr->mime, tr->rate, tr->ch);
        else printf("  [%d] %s", i, tr->mime);
    }
    printf("\n");
    return 1;
}

static void codec_close(Ext *e, int i) {
    if (e->codec[i]) { AMediaCodec_stop(e->codec[i]); AMediaCodec_delete(e->codec[i]); e->codec[i] = NULL; }
    q_clear(e, i);
}

static int ext_select(Ext *e, int i, int on) {
    if (!e->rd || i < 0 || i >= e->ntracks) return ST_INVALID;
    Track *tr = &e->tr[i];
    int ok = 1;
    if (on) {
        if (AMediaExtractor_selectTrack(e->rd, (size_t)i) != AMEDIA_OK) ok = 0;
        if (ok && tr->kind != 2 && !e->codec[i]) {
            AMediaCodec *cd = AMediaCodec_createDecoderByType(tr->mime);
            AMediaFormat *f = AMediaFormat_new();
            AMediaFormat_copy(f, e->fmt[i]);
            if (tr->kind == 0) AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);      /* COLOR_FormatYUV420SemiPlanar: NV12, what the game takes */
            media_status_t st = cd ? AMediaCodec_configure(cd, f, NULL, NULL, 0) : AMEDIA_ERROR_UNKNOWN;
            if (st == AMEDIA_OK) st = AMediaCodec_start(cd);
            AMediaFormat_delete(f);
            if (st != AMEDIA_OK) {
                printf("movie: track %d (%s): no decoder on this device (media status %d); track left unselected\n", i, tr->mime, (int)st);
                if (cd) AMediaCodec_delete(cd);
                AMediaExtractor_unselectTrack(e->rd, (size_t)i);
                return ST_UNSUPPORTED;
            }
            e->codec[i] = cd;
            tr->cfmt = tr->kind == 0 ? 21 : 0; tr->stride = tr->w; tr->slice = tr->h; tr->cl = tr->ct = 0;
            tr->codec_eos = tr->in_eos = 0;
        }
    } else {
        AMediaExtractor_unselectTrack(e->rd, (size_t)i);
        codec_close(e, i);
    }
    if (e->have_pending && e->pend_track == i) release_pending(e);
    release_ahead(e, i);
    tr->selected = on && ok;
    tr->eos = 0;
    printf("movie: %s track %d (%s)%s\n", on ? "selected" : "unselected", i, tr->mime, ok ? "" : " FAILED");
    return ok ? ST_OK : ST_FAILED;
}

/* the codec's video layout -> packed NV12 at the track's size */
static MSample *video_sample(Track *tr, const uint8_t *p, size_t n) {
    int w = tr->w, h = tr->h, stride = tr->stride > 0 ? tr->stride : w, slice = tr->slice > 0 ? tr->slice : h;
    size_t ysize = (size_t)w * h, uvrows = (size_t)((h + 1) / 2), need = ysize + (size_t)w * uvrows;
    MSample *s = calloc(1, sizeof *s);
    s->data = malloc(need); s->size = need;
    const uint8_t *y0 = p + (size_t)tr->ct * stride + tr->cl;
    int planar = tr->cfmt == 19 || tr->cfmt == 0x7F420888;                   /* I420 (Google's software decoders in buffer mode) */
    for (int y = 0; y < h; y++) {
        size_t o = (size_t)y * stride;
        if (y0 + o + w <= p + n) memcpy(s->data + (size_t)y * w, y0 + o, (size_t)w); else memset(s->data + (size_t)y * w, 0, (size_t)w);
    }
    uint8_t *uv = s->data + ysize;
    if (!planar) {                                    /* NV12, Qualcomm's NV12 (stride and slice height aligned) */
        const uint8_t *u0 = p + (size_t)stride * slice + (size_t)(tr->ct / 2) * stride + (tr->cl & ~1);
        for (size_t y = 0; y < uvrows; y++) {
            const uint8_t *r = u0 + y * stride;
            if (r + w <= p + n) memcpy(uv + y * w, r, (size_t)w); else memset(uv + y * w, 128, (size_t)w);
        }
    } else {
        int cs = (stride + 1) / 2, ch = (slice + 1) / 2;
        const uint8_t *u0 = p + (size_t)stride * slice + (size_t)(tr->ct / 2) * cs + tr->cl / 2, *v0 = u0 + (size_t)cs * ch;
        for (size_t y = 0; y < uvrows; y++)
            for (int x = 0; x < (w + 1) / 2; x++) {
                const uint8_t *pu = u0 + y * cs + x, *pv = v0 + y * cs + x;
                uv[y * w + 2 * x] = pu < p + n ? *pu : 128;
                if (2 * x + 1 < w) uv[y * w + 2 * x + 1] = pv < p + n ? *pv : 128;
            }
    }
    return s;
}

static void read_out_format(Ext *e, int t) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(e->codec[t]);
    if (!f) return;
    Track *tr = &e->tr[t];
    int32_t v;
    if (tr->kind == 0) {
        if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v)) tr->cfmt = v;
        if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v > 0) tr->stride = v;
        if (AMediaFormat_getInt32(f, "slice-height", &v) && v > 0) tr->slice = v;
        int32_t l = 0, tp = 0, r = 0, b = 0;
        if (AMediaFormat_getRect(f, AMEDIAFORMAT_KEY_DISPLAY_CROP, &l, &tp, &r, &b)) { tr->cl = l; tr->ct = tp; }
        printf("movie: track %d decodes to colour format 0x%x, stride %d, slice height %d, crop at %d,%d (shown %dx%d)\n", t, tr->cfmt, tr->stride, tr->slice, tr->cl, tr->ct, tr->w, tr->h);
    } else {
        if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v) && v > 0) tr->rate = v;
        if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v) && v > 0) tr->ch = v;
    }
    AMediaFormat_delete(f);
}

/* takes what codec t has decoded (waiting at most timeout_us); returns 1 when something happened */
static int drain(Ext *e, int t, int64_t timeout_us) {
    AMediaCodec *cd = e->codec[t];
    if (!cd || e->tr[t].codec_eos || e->qn[t] >= QMAX) return 0;
    AMediaCodecBufferInfo info;
    ssize_t ob = AMediaCodec_dequeueOutputBuffer(cd, &info, timeout_us);
    if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { read_out_format(e, t); return 1; }
    if (ob == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return 1;
    if (ob < 0) return 0;
    if (info.size > 0) {
        size_t cap = 0;
        uint8_t *p = AMediaCodec_getOutputBuffer(cd, (size_t)ob, &cap);
        if (p) {
            MSample *s;
            if (e->tr[t].kind == 0) s = video_sample(&e->tr[t], p + info.offset, (size_t)info.size);
            else { s = calloc(1, sizeof *s); s->data = malloc((size_t)info.size); s->size = (size_t)info.size; memcpy(s->data, p + info.offset, (size_t)info.size); }
            int k = (e->qh[t] + e->qn[t]) % QMAX;
            e->q[t][k].s = s; e->q[t][k].us = info.presentationTimeUs; e->qn[t]++;
            e->samples_read++;
        }
    }
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) e->tr[t].codec_eos = 1;
    AMediaCodec_releaseOutputBuffer(cd, (size_t)ob, false);
    return 1;
}

/* feeds the extractor's next sample to its track's codec (or the end of the stream to every codec) */
static int feed(Ext *e) {
    if (e->src_eos) {
        int any = 0;
        for (int t = 0; t < e->ntracks; t++) {
            if (!e->tr[t].selected || !e->codec[t] || e->tr[t].in_eos) continue;
            ssize_t ib = AMediaCodec_dequeueInputBuffer(e->codec[t], 2000);
            if (ib >= 0) { AMediaCodec_queueInputBuffer(e->codec[t], (size_t)ib, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); e->tr[t].in_eos = 1; any = 1; }
        }
        return any;
    }
    ssize_t t = AMediaExtractor_getSampleTrackIndex(e->rd);
    if (t < 0) { e->src_eos = 1; return 1; }
    if (t >= e->ntracks || !e->tr[t].selected || !e->codec[t]) { AMediaExtractor_advance(e->rd); return 1; }
    ssize_t ib = AMediaCodec_dequeueInputBuffer(e->codec[t], 2000);
    if (ib < 0) return 0;
    size_t cap = 0;
    uint8_t *buf = AMediaCodec_getInputBuffer(e->codec[t], (size_t)ib, &cap);
    ssize_t n = buf ? AMediaExtractor_readSampleData(e->rd, buf, cap) : -1;
    int64_t ts = AMediaExtractor_getSampleTime(e->rd);
    AMediaCodec_queueInputBuffer(e->codec[t], (size_t)ib, 0, (size_t)(n > 0 ? n : 0), (uint64_t)(ts > 0 ? ts : 0), 0);
    if (!AMediaExtractor_advance(e->rd)) e->src_eos = 1;
    return 1;
}

/* the next decoded sample of track want (0: none, the track has ended) */
static int pump_track(Ext *e, int want) {
    for (int guard = 0; guard < 20000; guard++) {
        if (e->qn[want]) return 1;
        if (e->tr[want].codec_eos || e->tr[want].failed) return 0;
        int moved = 0;
        for (int t = 0; t < e->ntracks; t++) if (e->tr[t].selected) while (drain(e, t, 0)) moved = 1;
        if (e->qn[want]) return 1;
        moved |= feed(e);
        if (!moved) drain(e, want, 5000);
    }
    printf("movie: track %d gives no decoded sample (codec stuck): treated as its end\n", want);
    e->tr[want].failed = 1;
    return 0;
}

/* The current sample of the extractor, as NuMediaExtractor defines it: the one with the SMALLEST TIMESTAMP among the
 * next decoded samples of the selected tracks (see nnmovie_mf.c). Returns 0 with a pending sample, or ST_END.
 * Caller holds the lock. */
static int ext_fill(Ext *e) {
    if (e->have_pending) return ST_OK;
    if (e->eos || !e->rd) return ST_END;
    int best = -1;
    for (int i = 0; i < e->ntracks; i++) {
        Track *tr = &e->tr[i];
        if (!tr->selected) continue;
        if (!e->ahead[i].have && !tr->eos) {
            if (pump_track(e, i)) {
                e->ahead[i].have = 1; e->ahead[i].s = e->q[i][e->qh[i]].s; e->ahead[i].us = e->q[i][e->qh[i]].us;
                e->qh[i] = (e->qh[i] + 1) % QMAX; e->qn[i]--;
            } else tr->eos = 1;
        }
        if (e->ahead[i].have && (best < 0 || e->ahead[i].us < e->ahead[best].us)) best = i;
    }
    if (best < 0) { if (!e->eos) printf("movie: end of stream after %llu samples\n", e->samples_read); e->eos = 1; return ST_END; }
    e->have_pending = 1; e->pending = e->ahead[best].s; e->pend_track = best; e->pend_us = e->ahead[best].us; e->pend_consumed = 0;
    return ST_OK;
}

/* ---------------------------------------------------------------- Extractor virtual functions */
static void ext_dtor_common(Ctx *c, int deleting) {
    Ext *e = ext_of(A0);
    if (e) {
        ext_lock(e);
        release_pending(e);
        release_ahead(e, -1);
        for (int i = 0; i < MAX_TRACKS; i++) { codec_close(e, i); if (e->fmt[i]) { AMediaFormat_delete(e->fmt[i]); e->fmt[i] = NULL; } }
        if (e->rd) AMediaExtractor_delete(e->rd);
        if (e->ds) AMediaDataSource_delete(e->ds);
        free(e->bytes);
        e->rd = NULL; e->ds = NULL; e->bytes = NULL;
        ext_unlock(e);
        plat_mutex_destroy(&e->cs);
        printf("movie: Extractor destroyed (%llu samples read)\n", e->samples_read);
        free(e);
        wr64(A0 + 8, 0);
    }
    if (deleting) gcall(c, GAME_DELETE, A0, 0, 0, 0, 0);     /* SMO: the game allocated the 16-byte object with its operator new
                                                               (_Znwm, sead heaps: MovieDecoderPlayer::CreateDecoders / CreateExtractor) */
}
static void ext_dtor(Ctx *c) { ext_dtor_common(c, 0); }
static void ext_deleting_dtor(Ctx *c) { ext_dtor_common(c, 1); }

static int ext_set_source_bytes(Ext *e, unsigned char *data, size_t size) {
    const char *save = getenv("SMO_MOVIE_SAVE");
    if (save) { FILE *f = fopen(save, "wb"); if (f) { fwrite(data, 1, size, f); fclose(f); printf("movie: saved %zu bytes to %s\n", size, save); } }
    int ok = ext_open_bytes(e, data, size);           /* Android: the data source reads from data: freed with the extractor */
    if (!ok && e->bytes != data) free(data);
    return ok ? ST_OK : ST_FAILED;
}

static void ext_SetDataSource_reader(Ctx *c) {       /* (ClientStreamReader *reader, const char *path) */
    Ext *e = ext_of(A0);
    uint64_t reader = A1;
    const char *path = (const char *)(uintptr_t)A2;
    if (!e || !reader) { RET(ST_INVALID); return; }
    strncpy(e->path, path ? path : "", sizeof e->path - 1);
    uint64_t vt = rd64(reader);
    uint64_t handle = 0;
    uint64_t ok = gcall(c, rd64(vt + 0x10), reader, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&handle, 0, 0);
    if (!(ok & 1)) { printf("movie: SetDataSource(%s): the game's reader could not open it\n", path); RET(ST_FAILED); return; }
    int64_t size = 0;
    uint64_t r = gcall(c, rd64(vt + 0x28), reader, handle, (uint64_t)(uintptr_t)&size, 0, 0);
    if (size <= 0 || size > (int64_t)2 << 30) {       /* the SDK ignores GetSize's return value [subsdk 0x1d93dc] */
        printf("movie: SetDataSource(%s): GetSize -> %llx, size %lld\n", path, (unsigned long long)r, (long long)size);
        gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0);
        RET(ST_FAILED); return;
    }
    unsigned char *data = malloc((size_t)size);
    if (!data) { gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0); RET(ST_FAILED); return; }
    int64_t got = 0;
    plat_li f0, t0, t1; ((f0).QuadPart = plat_qpf()); ((t0).QuadPart = plat_qpc());
    while (got < size) {
        uint64_t chunk = (uint64_t)(size - got) < (8u << 20) ? (uint64_t)(size - got) : (8u << 20);
        int64_t n = (int64_t)gcall(c, rd64(vt + 0x18), reader, handle, (uint64_t)got, (uint64_t)(uintptr_t)(data + got), chunk);
        if (n <= 0) break;
        got += n;
    }
    ((t1).QuadPart = plat_qpc());
    gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0);
    printf("movie: SetDataSource(%s): read %lld of %lld bytes through the game's reader in %.2f s\n", path, (long long)got, (long long)size,
           (double)(t1.QuadPart - t0.QuadPart) / f0.QuadPart);
    if (got != size) { free(data); RET(ST_FAILED); return; }
    RET(ext_set_source_bytes(e, data, (size_t)size));
}

static void ext_SetDataSource_path(Ctx *c) {         /* (const char *path): an nn::fs path (SMO: "content:/MovieData/x.mp4"), or a host file for tests */
    Ext *e = ext_of(A0);
    const char *path = (const char *)(uintptr_t)A1;
    char host[PLAT_MAX_PATH];
    int fs_host_path(const char *guest, char *out, size_t n);       /* nnfs.c */
    if (!e || !path) { RET(ST_INVALID); return; }
    strncpy(e->path, path, sizeof e->path - 1);
    FILE *f = fopen(fs_host_path(path, host, sizeof host) ? host : path, "rb");
    if (!f) { printf("movie: SetDataSource(%s): not a host file\n", path); RET(ST_FAILED); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)sz);
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    RET(got == (size_t)sz ? ext_set_source_bytes(e, data, got) : ST_FAILED);
}

static void ext_SetDataSource_mem(Ctx *c) {          /* (const void *data, int size) */
    Ext *e = ext_of(A0);
    if (!e || !A1 || (int)A2 <= 0) { RET(ST_INVALID); return; }
    strcpy(e->path, "memory.webm");
    unsigned char *data = malloc((size_t)(int)A2);
    memcpy(data, (void *)(uintptr_t)A1, (size_t)(int)A2);
    RET(ext_set_source_bytes(e, data, (size_t)(int)A2));
}

static void ext_GetTrackCount(Ctx *c) { Ext *e = ext_of(A0); if (!e || !A1) { RET(ST_INVALID); return; } wr32(A1, (uint32_t)(e->rd ? e->ntracks : 0)); RET(e->rd ? ST_OK : ST_FAILED); }

static void ext_GetTrackConfiguration(Ctx *c) {       /* (int track, MediaData *out) */
    Ext *e = ext_of(A0);
    int i = (int)A1;
    MD *m = md_of(A2);
    if (!e || !m) { RET(ST_INVALID); return; }
    if (!e->rd || i < 0 || i >= e->ntracks) { RET(ST_INVALID); return; }
    Track *tr = &e->tr[i];
    md_clear(m);
    md_set_str(m, "mime", tr->mime);
    md_set_i64(m, "durationUs", e->duration_us);
    md_set_i32(m, "max-input-size", 0x10000);
    if (tr->kind == 0) {
        md_set_i32(m, "width", tr->w); md_set_i32(m, "height", tr->h);
        md_set_i32(m, "frame-rate", (int)(tr->fps + 0.5));
    } else if (tr->kind == 1) {
        md_set_i32(m, "sample-rate", tr->rate); md_set_i32(m, "channel-count", tr->ch);
    }
    md_set_ptr(m, "nn-extractor", (uint64_t)(uintptr_t)e);    /* lets the decoder find its source */
    md_set_i32(m, "nn-track", i);
    if (g_log) printf("movie: GetTrackConfiguration(%d) -> %s\n", i, tr->mime);
    RET(ST_OK);
}

static void ext_SelectTrack(Ctx *c) { Ext *e = ext_of(A0); if (!e) { RET(ST_INVALID); return; } ext_lock(e); int r = ext_select(e, (int)A1, 1); ext_unlock(e); RET(r); }
static void ext_UnselectTrack(Ctx *c) { Ext *e = ext_of(A0); if (!e) { RET(ST_INVALID); return; } ext_lock(e); int r = ext_select(e, (int)A1, 0); ext_unlock(e); RET(r); }

static void ext_SeekTo(Ctx *c) {                      /* (int64 timeUs) */
    Ext *e = ext_of(A0);
    if (!e || !e->rd) { RET(ST_INVALID); return; }
    ext_lock(e);
    release_pending(e);
    release_ahead(e, -1);
    e->gen++;
    media_status_t hr = AMediaExtractor_seekTo(e->rd, (int64_t)A1, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
    e->eos = 0; e->src_eos = 0;
    for (int i = 0; i < e->ntracks; i++) {
        Track *tr = &e->tr[i];
        tr->eos = tr->codec_eos = tr->in_eos = 0;
        q_clear(e, i);
        if (e->codec[i]) AMediaCodec_flush(e->codec[i]);
    }
    ext_unlock(e);
    printf("movie: SeekTo(%.3f s) %d\n", (int64_t)A1 / 1e6, (int)hr);
    RET(hr == AMEDIA_OK ? ST_OK : ST_FAILED);
}

static void ext_GetTrackIndexForAvailableData(Ctx *c) {   /* (size_t *track) */
    Ext *e = ext_of(A0);
    if (!e || !A1) { RET(ST_INVALID); return; }
    ext_lock(e);
    int r = ext_fill(e);
    if (r == ST_OK) wr64(A1, (uint64_t)e->pend_track);
    ext_unlock(e);
    RET(r);
}

static void ext_Read(Ctx *c) {                        /* (Buffer *out): the sample token goes into the decoder's input slot */
    Ext *e = ext_of(A0);
    uint64_t b = A1;
    if (!e || !b) { RET(ST_INVALID); return; }
    ext_lock(e);
    int r = ext_fill(e);
    if (r == ST_OK) {
        uint64_t base = rd64(b + B_BASE), cap = rd64(b + B_CAP);
        if (base && cap >= sizeof(Token) && !e->pend_consumed) {
            Token t = {TOKEN_MAGIC, e->pending, e->pend_us, e->gen};
            memcpy((void *)(uintptr_t)base, &t, sizeof t);
            wr32(b + B_OFFSET, 0);
            wr32(b + B_SIZE, (uint32_t)sizeof t);
            wr64(b + B_I64, (uint64_t)e->pend_us);
            e->pend_consumed = 1;                     /* the sample now belongs to the token */
        } else {
            r = ST_FAILED;
        }
    }
    ext_unlock(e);
    RET(r);
}

static void ext_Advance(Ctx *c) {
    Ext *e = ext_of(A0);
    if (!e) { RET(ST_INVALID); return; }
    ext_lock(e);
    release_pending(e);
    ext_unlock(e);
    RET(ST_OK);
}

static void ext_GetSampleTime(Ctx *c) {               /* (int64 *timeUs) */
    Ext *e = ext_of(A0);
    if (!e || !A1) { RET(ST_INVALID); return; }
    ext_lock(e);
    int r = e->have_pending ? ST_OK : ext_fill(e);
    wr64(A1, (uint64_t)(r == ST_OK ? e->pend_us : -1));
    ext_unlock(e);
    RET(r);
}

static void ext_GetSampleMetaData(Ctx *c) {           /* (MediaData *) */
    Ext *e = ext_of(A0);
    MD *m = md_of(A1);
    if (!e || !m) { RET(ST_INVALID); return; }
    ext_lock(e);
    md_clear(m);
    if (e->have_pending) { md_set_i64(m, "timeUs", e->pend_us); md_set_i32(m, "track-index", e->pend_track); md_set_i32(m, "is-sync-frame", 1); }
    ext_unlock(e);
    RET(ST_OK);
}

static void ext_GetBufferedRange(Ctx *c) {            /* (int64 *startUs, int64 *endUs) */
    Ext *e = ext_of(A0);
    if (!e) { RET(ST_INVALID); return; }
    if (A1) wr64(A1, 0);
    if (A2) wr64(A2, (uint64_t)e->duration_us);
    RET(ST_OK);
}

/* ---------------------------------------------------------------- Decoder virtual functions */
static void dec_lock(Dec *d) { plat_mutex_lock(&d->cs); }
static void dec_unlock(Dec *d) { plat_mutex_unlock(&d->cs); }

static void dlog(Dec *d, const char *fmt, ...) {      /* SMO_MOVIE_LOG: one line per call, with thread and time */
    if (!g_log) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("movie[%5lu %8llu] %c %s\n", plat_tid(), (unsigned long long)plat_ms64(), d->is_video ? 'V' : 'A', line);
}

/* Frees every sample the decoder holds and empties all lists (caller holds the lock). */
static void dec_release_all(Dec *d) {
    for (int i = 0; i < d->nout; i++) { if (d->out[i].s) IMFSample_Release(d->out[i].s); memset(&d->out[i], 0, sizeof d->out[i]); }
    for (int i = 0; i < d->nin; i++) {
        if (d->in_state[i] == IN_DECODING && d->in_data[i].s) IMFSample_Release(d->in_data[i].s);
        Token t;                                      /* a sample read into the buffer but not sent yet */
        memcpy(&t, d->mem + (size_t)i * SLOT_BYTES, sizeof t);
        if (t.magic == TOKEN_MAGIC && t.s) IMFSample_Release(t.s);
        memset(d->mem + (size_t)i * SLOT_BYTES, 0, SLOT_BYTES);
        memset(&d->in_data[i], 0, sizeof d->in_data[i]);
        d->in_state[i] = IN_CODEC;
    }
    d->in_n = d->dec_n = d->out_n = 0;
}

/* The codec's own progress (caller holds the lock): every input waiting in decode moves to a free output buffer,
 * and its input buffer is offered again. Returns bit 0 = input indices were offered, bit 1 = frames became ready. */
static int dec_pump(Dec *d) {
    int ev = 0;
    while (d->dec_n > 0) {
        int o = -1;
        for (int i = 0; i < d->nout; i++) if (d->out[i].state == OUT_FREE) { o = i; break; }
        if (o < 0) break;
        int in = d->dec_list[0];
        memmove(d->dec_list, d->dec_list + 1, (size_t)(d->dec_n - 1) * sizeof d->dec_list[0]);
        d->dec_n--;
        d->out[o].s = d->in_data[in].s; d->out[o].ts = d->in_data[in].ts; d->out[o].flags = d->in_data[in].flags;
        d->out[o].state = OUT_READY;
        d->out_list[d->out_n++] = o;
        memset(&d->in_data[in], 0, sizeof d->in_data[in]);
        d->in_state[in] = IN_OFFERED;
        d->in_list[d->in_n++] = in;
        ev |= 3;
    }
    return ev;
}
static void dec_signal(Dec *d, int ev) { if (ev & 2) signal_event(d, 1); if (ev & 1) signal_event(d, 0); }

static void dec_dtor_common(Ctx *c, int deleting) {
    Dec *d = dec_of(A0);
    if (d) {
        dec_lock(d);
        dec_release_all(d);
        dec_unlock(d);
        plat_mutex_destroy(&d->cs);
        printf("movie: Decoder(%s) destroyed: %llu input buffers, %llu output buffers, %llu copied to the game, %llu refused, %llu flushes\n",
               d->is_video ? "video" : "audio", d->in_count, d->out_count, d->copied, d->dropped, d->flushes);
        free(d->mem);
        free(d);
        wr64(A0 + 8, 0);
    }
    if (deleting) gcall(c, GAME_DELETE, A0, 0, 0, 0, 0);     /* SMO: the game allocated the 16-byte object with its operator new
                                                               (_Znwm, sead heaps: MovieDecoderPlayer::CreateDecoders / CreateExtractor) */
}
static void dec_dtor(Ctx *c) { dec_dtor_common(c, 0); }
static void dec_deleting_dtor(Ctx *c) { dec_dtor_common(c, 1); }

static void dec_Configure(Ctx *c) {                   /* (MediaData *format) */
    Dec *d = dec_of(A0);
    MD *m = md_of(A1);
    if (!d) { RET(ST_INVALID); return; }
    uint64_t ext = 0; int32_t track = -1, v = 0;
    const char *mime = "?";
    if (m) {
        md_get_ptr(m, "nn-extractor", &ext);
        md_get_i32(m, "nn-track", &track);
        md_get_str(m, "mime", &mime);
        if (md_get_i32(m, "width", &v)) d->w = v;
        if (md_get_i32(m, "height", &v)) d->h = v;
        if (md_get_i32(m, "sample-rate", &v)) d->rate = v;
        if (md_get_i32(m, "channel-count", &v)) d->ch = v;
    }
    d->ext = (Ext *)(uintptr_t)ext;
    d->track = track;
    printf("movie: Decoder(%s type %d, output format %d) configured for track %d (%s%s, %dx%d, %d Hz, %d ch)\n",
           d->is_video ? "video" : "audio", d->type, d->fmt, track, mime, ext ? "" : ", NO extractor link, ", d->w, d->h, d->rate, d->ch);
    if (!ext) { RET(ST_UNSUPPORTED); return; }
    RET(ST_OK);                                       /* input buffers are offered by Start, not here */
}

static void dec_RegisterClientEvents(Ctx *c) {
    Dec *d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    dec_lock(d);
    d->events = A1;
    int ev = (d->in_n > 0 ? 1 : 0) | (d->out_n > 0 ? 2 : 0);     /* registered after Start: nothing may be lost */
    dec_unlock(d);
    dec_signal(d, ev);
    RET(ST_OK);
}

static void dec_fill_format(Dec *d, MD *m) {
    md_clear(m);
    if (d->is_video) {
        int w = d->w, h = d->h;
        md_set_str(m, "mime", "video/raw");
        md_set_i32(m, "width", w); md_set_i32(m, "height", h);
        md_set_i32(m, "stride", w); md_set_i32(m, "slice-height", h);
        md_set_i32(m, "crop-width", w); md_set_i32(m, "crop-height", h);
        md_set_i32(m, "color-format", 21);            /* OMX_COLOR_FormatYUV420SemiPlanar = NV12 */
        md_set_i32(m, "nv12-width", w); md_set_i32(m, "nv12-height", h);
        md_set_i32(m, "nv12-y-stride", w); md_set_i32(m, "nv12-uv-stride", w);
        md_set_i32(m, "nv12-y-offset", 0); md_set_i32(m, "nv12-uv-offset", w * h);
        md_set_i32(m, "nv12-colorspace", 0);
    } else {
        md_set_str(m, "mime", "audio/raw");
        md_set_i32(m, "sample-rate", d->rate); md_set_i32(m, "channel-count", d->ch);
        md_set_i32(m, "pcm-encoding", 2);             /* 16-bit */
    }
}

static void dec_GetOutputFormat(Ctx *c) { Dec *d = dec_of(A0); MD *m = md_of(A1); if (!d || !m) { RET(ST_INVALID); return; } dec_fill_format(d, m); RET(ST_OK); }
static void dec_GetOutputBufferFormat(Ctx *c) { Dec *d = dec_of(A0); MD *m = md_of(A2); if (!d || !m) { RET(ST_INVALID); return; } dec_fill_format(d, m); RET(ST_OK); }

/* P3 round 3: which decoders send the "output format" event (DecoderEvents + 0x10). The game's handler thread
 * [game 0x71036f3500] signals the player's "formats known" event (player + 0x498) only when the format event of EVERY
 * decoder it created has arrived (cases 3 and 7 of its switch: flags + 0x4c1 and + 0x4c0), and the playback thread
 * waits for it for 5 s while holding the player's mutex [0x71036f2580]: with the event sent by the video decoder only,
 * every movie start waited the full 5 s and the main thread stood at that mutex [0x71036e7690]. So the real audio
 * decoder sends it as well. SMO_MOVIE_NO_AUDIO_FORMAT_EVENT=1: video only, as before. */
static int fmt_event_wanted(const Dec *d) {
    static int video_only = -1;
    if (video_only < 0) video_only = getenv("SMO_MOVIE_NO_AUDIO_FORMAT_EVENT") != NULL;
    return (d->is_video || !video_only) && !d->fmt_signalled;
}
static void dec_Start(Ctx *c) {
    Dec *d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    dec_lock(d);
    int offer = !d->started || d->flushed;            /* first start, or the start that follows a flush: all input buffers are offered */
    if (offer) {
        dec_release_all(d);
        for (int i = 0; i < d->nin; i++) { d->in_state[i] = IN_OFFERED; d->in_list[i] = i; }
        d->in_n = d->nin;
    }
    d->started = 1; d->flushed = 0;
    int fmt = fmt_event_wanted(d);
    if (fmt) d->fmt_signalled = 1;
    dec_unlock(d);
    dlog(d, "Start%s", offer ? " (all input buffers offered)" : " (already running)");
    if (fmt) signal_event(d, 2);
    if (offer) signal_event(d, 0);
    RET(ST_OK);
}
static void dec_halt(Ctx *c, const char *what, int stop) {
    Dec *d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    dec_lock(d);
    int held_in = 0, held_out = 0;
    for (int i = 0; i < d->nin; i++) held_in += d->in_state[i] == IN_CLIENT;
    for (int i = 0; i < d->nout; i++) held_out += d->out[i].state == OUT_CLIENT;
    int queued = d->out_n + d->dec_n;
    dec_release_all(d);
    d->flushed = 1; d->flushes++;
    if (stop) d->started = 0;
    dec_unlock(d);
    dlog(d, "%s: dropped %d decoded frames, voided %d input and %d output indices held by the game", what, queued, held_in, held_out);
    RET(ST_OK);                                       /* no event: nothing is offered until Start */
}
static void dec_Stop(Ctx *c) { dec_halt(c, "Stop", 1); }
static void dec_Flush(Ctx *c) { dec_halt(c, "Flush", 0); }
static void dec_Reset(Ctx *c) { dec_halt(c, "Reset", 1); }
/* [subsdk 0x21090] Decoder::GetLastError: 0x1000 without an implementation object, else DecoderImpl +0xd8 [0x1fc80]. That field is
 * written by the codec's message receiver [0x203a8..0x20410]: on an error message the status (0x1000 unless the codec's own code
 * converts to another one [0x35e140]) is stored and the client's error event (+0x18) is signalled [0x20428]. Here the codec is Media
 * Foundation's, inside the source reader: a failed read of the track is that error; its HRESULT has no movie::Status, so 0x1000. */
static void dec_error_check(Dec *d) {                 /* called with the decoder locked */
    if (d->last_error || !d->ext || d->track < 0 || d->track >= d->ext->ntracks || !d->ext->tr[d->track].failed) return;
    d->last_error = ST_FAILED;
    printf("movie: decoder of track %d: codec error -> last error 0x%x, error event signalled\n", d->track, ST_FAILED);
    signal_event(d, 3);
}
static void dec_GetLastError(Ctx *c) { Dec *d = dec_of(A0); RET(d ? d->last_error : ST_FAILED); }

static void dec_AcquireInputBufferIndex(Ctx *c) {     /* (int *index, int *moreAvailable) */
    Dec *d = dec_of(A0);
    if (!d || !A1 || !A2) { RET(ST_INVALID); return; }     /* both pointers are required [subsdk 0x1fa48] */
    dec_lock(d);
    int found = -1, more = 0;
    if (d->in_n > 0) {
        found = d->in_list[0];
        memmove(d->in_list, d->in_list + 1, (size_t)(d->in_n - 1) * sizeof d->in_list[0]);
        more = --d->in_n;
        d->in_state[found] = IN_CLIENT;
    }
    dec_unlock(d);
    wr32(A1, (uint32_t)found);
    wr32(A2, (uint32_t)more);
    dlog(d, "AcquireInput -> %d (more %d)", found, more);
    RET(found >= 0 ? ST_OK : ST_NO_INPUT);
}

static void dec_GetInputBuffer(Ctx *c) {              /* (int index, Buffer *out) */
    Dec *d = dec_of(A0);
    int i = (int)A1;
    uint64_t b = A2;
    if (!d || !b || i < 0 || i >= d->nin) { RET(ST_INVALID); return; }
    buf_release(c, b);
    wr64(b + B_BASE, (uint64_t)(uintptr_t)(d->mem + (size_t)i * SLOT_BYTES));
    wr64(b + B_CAP, SLOT_BYTES);
    wr32(b + B_OFFSET, 0); wr32(b + B_SIZE, 0);
    RET(ST_OK);
}

static void dec_SendInputBufferForDecode(Ctx *c) {    /* (int index, int offset, int size, int64 timeUs, unsigned flags) */
    Dec *d = dec_of(A0);
    int i = (int)A1, size = (int)A3;                  /* A2 = offset, A3 = size, A4 = timeUs, x5 = flags [game 0x71036f6e2c] */
    int64_t ts = (int64_t)A4;
    uint32_t flags = (uint32_t)c->x[5];
    if (!d || i < 0 || i >= d->nin) { RET(ST_INVALID); return; }
    dec_lock(d);
    dec_error_check(d);
    IMFSample *sample = NULL;
    int stale = 0;
    Token t;
    memcpy(&t, d->mem + (size_t)i * SLOT_BYTES, sizeof t);
    memset(d->mem + (size_t)i * SLOT_BYTES, 0, sizeof t);
    if (t.magic == TOKEN_MAGIC && t.s) {
        sample = t.s;
        if (ts <= 0) ts = t.ts;
        if (d->ext && t.gen != d->ext->gen) stale = 1;      /* read before the last SeekTo */
    } else if (!(flags & FLAG_EOS) && !d->warned_token) {
        d->warned_token = 1;
        printf("movie: Decoder(%s): input buffer %d holds no sample token (size %d): dropped\n", d->is_video ? "video" : "audio", i, size);
    }
    if (d->in_state[i] != IN_CLIENT || stale) {       /* an index voided by Flush, or data from before the seek: the codec refuses it */
        if (sample) IMFSample_Release(sample);
        d->dropped++;
        int was = d->in_state[i];
        int ev = 0;
        if (was == IN_CLIENT) { d->in_state[i] = IN_OFFERED; d->in_list[d->in_n++] = i; ev = 1; }
        dec_unlock(d);
        dlog(d, "SendInput %d REFUSED (%s) t=%.3f flags %x", i, was != IN_CLIENT ? "index not held by the game" : "sample from before the seek", ts / 1e6, flags);
        dec_signal(d, ev);
        RET(was == IN_CLIENT ? ST_OK : ST_INVALID);
        return;
    }
    d->in_count++;
    if (SMO_UNPORTED && g_test_resync_at && d->is_video && d->in_count == (unsigned long long)g_test_resync_at) {
        /* SMO_MOVIE_TEST_RESYNC=<flag>,<n>: at the n-th video sample raise the game's own "re-synchronise movies" request
         * (byte 0x7107446040, set by its system-notification handler f_7103741700: 8 = operation mode changed, 4 / 2 = the
         * two halves of a focus change); the movie manager [game 0x71036e3984] then pauses, seeks and resumes the player. */
        uint8_t *req = (uint8_t *)(uintptr_t)0 /* SSBU 13.0.4 address */;
        plat_atomic_fetch_or((char *)req, (char)g_test_resync_flag);
        printf("movie: TEST: resync request %d raised at video sample %d\n", g_test_resync_flag, g_test_resync_at);
    }
    if (!sample && !(flags & FLAG_EOS)) {             /* nothing to decode: the buffer is offered again */
        d->dropped++;
        d->in_state[i] = IN_OFFERED; d->in_list[d->in_n++] = i;
        dec_unlock(d);
        signal_event(d, 0);
        RET(ST_OK);
        return;
    }
    d->in_data[i].s = sample; d->in_data[i].ts = ts; d->in_data[i].flags = flags & FLAG_EOS;
    d->in_state[i] = IN_DECODING;
    d->dec_list[d->dec_n++] = i;
    int first_fmt = fmt_event_wanted(d);
    if (first_fmt) d->fmt_signalled = 1;
    if (d->in_count == 1 || (flags & FLAG_EOS)) printf("movie: Decoder(%s): %s queued (t=%.3f s)\n", d->is_video ? "video" : "audio", flags & FLAG_EOS ? "end of stream" : "first sample", ts / 1e6);
    int ev = dec_pump(d);
    int waiting = d->dec_n, ready = d->out_n;
    dec_unlock(d);
    dlog(d, "SendInput %d t=%.3f flags %x (ready %d, waiting for an output buffer %d)", i, ts / 1e6, flags, ready, waiting);
    if (first_fmt) signal_event(d, 2);
    dec_signal(d, ev);
    RET(ST_OK);
}

static void dec_AcquireOutputBufferIndex(Ctx *c) {    /* (int *index, int64 *timeUs, unsigned *flags, int *moreAvailable) */
    Dec *d = dec_of(A0);
    if (!d || !A1 || !A2 || !A3 || !A4) { RET(ST_INVALID); return; }    /* all four are required [subsdk 0x1e730] */
    dec_lock(d);
    dec_error_check(d);
    int r = ST_NO_OUTPUT, idx = -1; int64_t ts = 0; uint32_t flags = 0; int more = 0;
    if (d->out_n > 0) {
        idx = d->out_list[0];
        memmove(d->out_list, d->out_list + 1, (size_t)(d->out_n - 1) * sizeof d->out_list[0]);
        more = --d->out_n;
        OutBuf *s = &d->out[idx];
        s->state = OUT_CLIENT; ts = s->ts; flags = s->flags;
        d->out_count++;
        r = ST_OK;
    }
    dec_unlock(d);
    wr32(A1, (uint32_t)idx);
    wr64(A2, (uint64_t)ts);
    wr32(A3, flags);
    wr32(A4, (uint32_t)more);
    dlog(d, "AcquireOutput -> %d t=%.3f flags %x (more %d)", idx, ts / 1e6, flags, more);
    RET(r);
}

/* Copies the decoded sample of slot i into the game's buffer; returns the number of bytes written. */
static size_t copy_sample(Dec *d, OutBuf *s, unsigned char *dst, size_t cap) {
    if (!s->s || !s->s->data) return 0;
    size_t n = s->s->size < cap ? s->s->size : cap;
    if (s->s->size > cap && !d->warned_small) { d->warned_small = 1; printf("movie: game buffer of %zu bytes is smaller than a decoded %s (%zu)\n", cap, d->is_video ? "frame" : "block", s->s->size); }
    if (d->is_video && s->s->size > cap) return 0;    /* a frame is all or nothing */
    memcpy(dst, s->s->data, n);
    return n;
}

static void dec_GetOutputBuffer(Ctx *c) {             /* (int index, Buffer *out[, MediaData *format]) */
    Dec *d = dec_of(A0);
    int i = (int)A1;
    uint64_t b = A2;
    if (!d || !b || i < 0 || i >= d->nout) { RET(ST_INVALID); return; }
    dec_lock(d);
    OutBuf *s = &d->out[i];
    int r = ST_OK;
    if (s->state != OUT_CLIENT) r = ST_INVALID;       /* not acquired, or voided by Flush */
    else {
        uint64_t base = rd64(b + B_BASE), cap = rd64(b + B_CAP);
        size_t n = base ? copy_sample(d, s, (unsigned char *)(uintptr_t)base, (size_t)cap) : 0;
        wr32(b + B_OFFSET, 0);
        wr32(b + B_SIZE, (uint32_t)n);
        wr64(b + B_I64, (uint64_t)s->ts);
        wr32(b + B_U32, s->flags);
        if (n) d->copied++;
        if (d->copied == 1 && n) printf("movie: Decoder(%s): first frame copied to the game (%zu bytes into a %llu-byte buffer at %llx, t=%.3f s)\n",
                                        d->is_video ? "video" : "audio", n, (unsigned long long)cap, (unsigned long long)base, s->ts / 1e6);
        else if (d->is_video && d->copied % 300 == 0 && n) printf("movie: %llu video frames delivered (t=%.1f s)\n", d->copied, s->ts / 1e6);
    }
    dec_unlock(d);
    if (r) dlog(d, "GetOutputBuffer %d REFUSED (not held by the game)", i);
    RET(r);
}

static void dec_GetOutputBufferWithFormat(Ctx *c) {
    Dec *d = dec_of(A0);
    MD *m = md_of(A3);
    if (d && m) dec_fill_format(d, m);
    dec_GetOutputBuffer(c);
}

static void dec_GetOutputBufferSize(Ctx *c) {         /* (int *size, int *count) */
    Dec *d = dec_of(A0);
    if (!d || !A1) { RET(ST_INVALID); return; }
    int size = d->is_video ? d->w * d->h + 2 * ((d->w + 1) / 2) * ((d->h + 1) / 2) : 0x8000;
    wr32(A1, (uint32_t)size);
    if (A2) wr32(A2, 1);
    RET(ST_OK);
}

static void dec_ReleaseOutputBufferIndex(Ctx *c) {    /* (int index) */
    Dec *d = dec_of(A0);
    int i = (int)A1;
    if (!d || i < 0 || i >= d->nout) { RET(ST_INVALID); return; }
    dec_lock(d);
    OutBuf *s = &d->out[i];
    int ev = 0, ok = s->state == OUT_CLIENT;
    if (ok) {
        if (s->s) IMFSample_Release(s->s);
        memset(s, 0, sizeof *s);
        ev = dec_pump(d);                             /* a waiting input can now be decoded into this buffer */
    }
    dec_unlock(d);
    dlog(d, "ReleaseOutput %d%s", i, ok ? "" : " IGNORED (not held by the game)");
    dec_signal(d, ev);
    RET(ST_OK);
}

/* ---------------------------------------------------------------- vtables and constructors */
static uint64_t g_ext_vtable[20], g_dec_vtable[20];
static plat_mutex g_init_lock;

typedef struct { const char *name; ImportFn fn; } Slot_;
static void build_vtables(void) {
    static const Slot_ ext_slots[] = {
        {"movie::Extractor::~Extractor", ext_dtor}, {"movie::Extractor::~Extractor(delete)", ext_deleting_dtor},
        {"movie::Extractor::SetDataSource(path)", ext_SetDataSource_path}, {"movie::Extractor::SetDataSource(memory)", ext_SetDataSource_mem},
        {"movie::Extractor::GetTrackCount", ext_GetTrackCount}, {"movie::Extractor::GetTrackConfiguration", ext_GetTrackConfiguration},
        {"movie::Extractor::SelectTrack", ext_SelectTrack}, {"movie::Extractor::UnselectTrack", ext_UnselectTrack},
        {"movie::Extractor::SeekTo", ext_SeekTo}, {"movie::Extractor::GetTrackIndexForAvailableData", ext_GetTrackIndexForAvailableData},
        {"movie::Extractor::Read", ext_Read}, {"movie::Extractor::Advance", ext_Advance},
        {"movie::Extractor::GetSampleTime", ext_GetSampleTime}, {"movie::Extractor::GetSampleMetaData", ext_GetSampleMetaData},
        {"movie::Extractor::GetBufferedRange", ext_GetBufferedRange}, {"movie::Extractor::SetDataSource(reader)", ext_SetDataSource_reader},
    };
    static const Slot_ dec_slots[] = {
        {"movie::Decoder::~Decoder", dec_dtor}, {"movie::Decoder::~Decoder(delete)", dec_deleting_dtor},
        {"movie::Decoder::Configure", dec_Configure}, {"movie::Decoder::RegisterClientEvents", dec_RegisterClientEvents},
        {"movie::Decoder::GetOutputFormat", dec_GetOutputFormat}, {"movie::Decoder::Start", dec_Start},
        {"movie::Decoder::Stop", dec_Stop}, {"movie::Decoder::Reset", dec_Reset}, {"movie::Decoder::Flush", dec_Flush},
        {"movie::Decoder::AcquireInputBufferIndex", dec_AcquireInputBufferIndex}, {"movie::Decoder::GetInputBuffer", dec_GetInputBuffer},
        {"movie::Decoder::SendInputBufferForDecode", dec_SendInputBufferForDecode}, {"movie::Decoder::AcquireOutputBufferIndex", dec_AcquireOutputBufferIndex},
        {"movie::Decoder::GetOutputBuffer", dec_GetOutputBuffer}, {"movie::Decoder::GetOutputBufferSize", dec_GetOutputBufferSize},
        {"movie::Decoder::ReleaseOutputBufferIndex", dec_ReleaseOutputBufferIndex}, {"movie::Decoder::GetLastError", dec_GetLastError},
        {"movie::Decoder::GetOutputBuffer(format)", dec_GetOutputBufferWithFormat}, {"movie::Decoder::GetOutputBufferFormat", dec_GetOutputBufferFormat},
    };
    plat_mutex_lock(&g_init_lock);
    if (!g_ext_vtable[0]) {
        for (unsigned i = 0; i < sizeof ext_slots / sizeof ext_slots[0]; i++) g_ext_vtable[i] = hostfn_address(ext_slots[i].name, ext_slots[i].fn);
        for (unsigned i = 0; i < sizeof dec_slots / sizeof dec_slots[0]; i++) g_dec_vtable[i] = hostfn_address(dec_slots[i].name, dec_slots[i].fn);
    }
    plat_mutex_unlock(&g_init_lock);
}

static void extractor_ctor(Ctx *c) {                  /* Extractor(ContainerType) */
    build_vtables();
    Ext *e = calloc(1, sizeof *e);
    plat_mutex_init(&e->cs);
    wr64(A0, (uint64_t)(uintptr_t)g_ext_vtable);
    wr64(A0 + 8, (uint64_t)(uintptr_t)e);
    input_mark("movie");
    printf("movie: Extractor created (container type %u: %s) on Media Foundation\n", (unsigned)A1, A1 == 1 ? "mp4" : A1 == 2 ? "mkv" : A1 == 3 ? "webm" : "?");
}

static void decoder_ctor(Ctx *c) {                    /* Decoder(DecoderType, DecoderOutputFormat, DecoderMode, int) */
    build_vtables();
    Dec *d = calloc(1, sizeof *d);
    plat_mutex_init(&d->cs);
    d->type = (int)A1; d->fmt = (int)A2; d->mode = (int)A3;
    d->is_video = d->type >= 1 && d->type <= 3;
    d->nin = 8;                                       /* compressed-data buffers: only bound how far the feed runs ahead of the codec */
    /* decoded frames the game may hold or leave queued before the codec stalls. SMO port: the game's VideoRenderer queues
     * at most 32 ([game] VideoRenderer::open: 0x200 bytes of 16-byte entries; MovieDecoderPlayer::VideoOutputBufferAvailableEvent
     * moves every ready frame into it, and onOutputAvailable writes through a null entry when it is full); it takes one
     * frame per update and releases it at once. 16 stays well inside that (half a second of 30 fps video). The 48 of
     * ssbu-recomp (its player queued more than a second before the first frame) made SMO's queue overflow. */
    d->nout = d->is_video ? 16 : 64;
    d->mem = calloc((size_t)d->nin, SLOT_BYTES);
    wr64(A0, (uint64_t)(uintptr_t)g_dec_vtable);
    wr64(A0 + 8, (uint64_t)(uintptr_t)d);
    printf("movie: Decoder created (type %d = %s, output format %d, mode %d, %d)\n", d->type,
           d->type == 1 ? "H.264" : d->type == 2 ? "VP8" : d->type == 3 ? "VP9" : d->type == 4 ? "AAC" : d->type == 5 ? "Vorbis" : "?",
           d->fmt, d->mode, (int)A4);
    if (d->is_video && d->fmt != 4) printf("movie: WARNING output format %d requested, only 4 (NV12) is produced\n", d->fmt);
}

/* ---------------------------------------------------------------- free functions */
static void movie_SetCoreMask(Ctx *c) { (void)c; }
static int g_thread_priority;
static void movie_SetThreadPriority(Ctx *c) { g_thread_priority = (int)A0; }
static void movie_GetThreadPriority(Ctx *c) { RET(g_thread_priority); }

/* H.264 helpers (the game's elementary-stream path, not used by the WebM movies). */
static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end) {
    for (; p + 3 <= end; p++) if (p[0] == 0 && p[1] == 0 && p[2] == 1) return p;
    return NULL;
}
static void movie_GetH264NalUnitStartPositionAndNalSize(Ctx *c) {   /* (uint8_t **pos, size_t *remaining, uint8_t **nalStart, size_t *nalSize) -> bool */
    uint64_t ppos = A0, prem = A1, pstart = A2, psize = A3;
    if (!ppos || !prem || !pstart || !psize) { RET(0); return; }
    const uint8_t *p = (const uint8_t *)(uintptr_t)rd64(ppos), *end = p + rd64(prem);
    const uint8_t *sc = p ? find_start_code(p, end) : NULL;
    if (!sc) { RET(0); return; }
    const uint8_t *nal = sc + 3;
    const uint8_t *next = find_start_code(nal, end);
    const uint8_t *nal_end = next ? next : end;
    while (nal_end > nal && nal_end[-1] == 0) nal_end--;      /* a 4-byte start code's leading zero belongs to the next unit */
    wr64(pstart, (uint64_t)(uintptr_t)nal);
    wr64(psize, (uint64_t)(nal_end - nal));
    wr64(ppos, (uint64_t)(uintptr_t)(next ? next : end));
    wr64(prem, (uint64_t)(end - (next ? next : end)));
    RET(1);
}
typedef struct { const uint8_t *p; size_t n; size_t bit; } BitReader;
static unsigned br_bit(BitReader *b) { if (b->bit >= b->n * 8) return 0; unsigned v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1; b->bit++; return v; }
static unsigned br_bits(BitReader *b, int n) { unsigned v = 0; while (n-- > 0) v = (v << 1) | br_bit(b); return v; }
static unsigned br_ue(BitReader *b) { int z = 0; while (!br_bit(b) && z < 32) z++; return z ? ((1u << z) - 1 + br_bits(b, z)) : 0; }
static int br_se(BitReader *b) { unsigned v = br_ue(b); return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2); }
static void movie_GetH264PropertiesFromSps(Ctx *c) {  /* (const uint8_t *sps, size_t len, int *width, int *height, double *frameRate) -> bool */
    const uint8_t *sps = (const uint8_t *)(uintptr_t)A0;
    size_t len = A1;
    if (!sps || len < 4) { RET(0); return; }
    uint8_t clean[512]; size_t n = 0;                 /* strip emulation prevention bytes */
    for (size_t i = 0; i < len && n < sizeof clean; i++) { if (i >= 2 && sps[i] == 3 && sps[i - 1] == 0 && sps[i - 2] == 0) continue; clean[n++] = sps[i]; }
    BitReader b = {clean, n, 0};
    if ((br_bits(&b, 8) & 0x1f) != 7) { RET(0); return; }
    unsigned profile = br_bits(&b, 8); br_bits(&b, 16); br_ue(&b);
    unsigned chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 || profile == 128) {
        chroma = br_ue(&b); if (chroma == 3) br_bit(&b);
        br_ue(&b); br_ue(&b); br_bit(&b);
        if (br_bit(&b)) for (int i = 0; i < (chroma != 3 ? 8 : 12); i++) if (br_bit(&b)) { int last = 8, nxt = 8; for (int j = 0; j < (i < 6 ? 16 : 64); j++) { if (nxt) { int delta = br_se(&b); nxt = (last + delta + 256) % 256; } last = nxt ? nxt : last; } }
    }
    br_ue(&b);
    unsigned poc = br_ue(&b);
    if (poc == 0) br_ue(&b);
    else if (poc == 1) { br_bit(&b); br_se(&b); br_se(&b); unsigned k = br_ue(&b); for (unsigned i = 0; i < k && i < 256; i++) br_se(&b); }
    br_ue(&b); br_bit(&b);
    unsigned wmb = br_ue(&b) + 1, hmap = br_ue(&b) + 1, frame_mbs_only = br_bit(&b);
    if (!frame_mbs_only) br_bit(&b);
    br_bit(&b);
    unsigned cl = 0, cr = 0, ct = 0, cb = 0;
    if (br_bit(&b)) { cl = br_ue(&b); cr = br_ue(&b); ct = br_ue(&b); cb = br_ue(&b); }
    unsigned sub_w = chroma == 3 ? 1 : 2, sub_h = chroma == 1 ? 2 : 1, unit_y = sub_h * (2 - frame_mbs_only);
    int width = (int)(wmb * 16 - sub_w * (cl + cr)), height = (int)((2 - frame_mbs_only) * hmap * 16 - unit_y * (ct + cb));
    double fps = 0.0;
    if (br_bit(&b)) {                                 /* VUI */
        if (br_bit(&b)) { if (br_bits(&b, 8) == 255) br_bits(&b, 32); }
        if (br_bit(&b)) br_bit(&b);
        if (br_bit(&b)) { br_bits(&b, 4); if (br_bit(&b)) br_bits(&b, 24); }
        if (br_bit(&b)) { br_ue(&b); br_ue(&b); }
        if (br_bit(&b)) { unsigned num = br_bits(&b, 32), den = br_bits(&b, 32); if (num) fps = (double)den / (2.0 * num); }
    }
    if (A2) wr32(A2, (uint32_t)width);
    if (A3) wr32(A3, (uint32_t)height);
    if (A4) { double f = fps; uint64_t bits; memcpy(&bits, &f, 8); wr64(A4, bits); }
    RET(1);
}
static void movie_CreateH264DecoderConfigurationData(Ctx *c) {   /* (sps, spsLen, pps, ppsLen, MediaData *out) */
    MD *m = md_of(A4);
    if (!m) { RET(ST_INVALID); return; }
    int w = 0, h = 0;
    md_set_str(m, "mime", "video/avc");
    md_set_i64(m, "csd-0-ptr", A0); md_set_i64(m, "csd-0-size", A1);
    md_set_i64(m, "csd-1-ptr", A2); md_set_i64(m, "csd-1-size", A3);
    (void)w; (void)h;
    printf("movie: CreateH264DecoderConfigurationData(sps %llu bytes, pps %llu bytes): H.264 elementary streams are not implemented\n", (unsigned long long)A1, (unsigned long long)A3);
    RET(ST_UNSUPPORTED);
}

/* ---------------------------------------------------------------- registration */
/* SMO (SDK 3.5.1, its movie library in subsdk0): Extractor(ContainerType, CacheSize) and Decoder(DecoderType,
 * DecoderOutputFormat) [subsdk0 0x2e04f8, 0x2de630]; the vtables are the ones listed at the top, slot for slot (read off
 * SMO's _ZTVN5movie9ExtractorE / _ZTVN5movie7DecoderE). The cache size and the absent mode are not used here. */
static void extractor_ctor_cache(Ctx *c) { c->x[2] = 0; extractor_ctor(c); }
static void decoder_ctor_nomode(Ctx *c) { c->x[3] = 0; c->x[4] = 0; decoder_ctor(c); }
static const ImportDef movie_mf_imports[] = {
    {"_ZN5movie9ExtractorC1ENS_13ContainerTypeE", extractor_ctor},
    {"_ZN5movie9ExtractorC1ENS_13ContainerTypeENS_9CacheSizeE", extractor_ctor_cache}, {"_ZN5movie9ExtractorC2ENS_13ContainerTypeENS_9CacheSizeE", extractor_ctor_cache},
    {"_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatE", decoder_ctor_nomode}, {"_ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatE", decoder_ctor_nomode},
    {"_ZN5movie9ExtractorC2ENS_13ContainerTypeE", extractor_ctor},
    {"_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", decoder_ctor},
    {"_ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", decoder_ctor},
    {"_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", movie_SetAllocator},
    {"_ZN5movie11SetCoreMaskEm", movie_SetCoreMask},
    {"_ZN5movie17SetThreadPriorityEi", movie_SetThreadPriority},
    {"_ZN5movie17GetThreadPriorityEv", movie_GetThreadPriority},
    {"_ZN5movie34CreateH264DecoderConfigurationDataEPKhmS1_mPNS_9MediaDataE", movie_CreateH264DecoderConfigurationData},
    {"_ZN5movie37GetH264NalUnitStartPositionAndNalSizeEPPhPmS1_S2_", movie_GetH264NalUnitStartPositionAndNalSize},
    {"_ZN5movie37GetH264PropertiresFromSeqParamSetDataEPKhmPiS2_Pd", movie_GetH264PropertiesFromSps},
    /* movie::Buffer */
    {"_ZN5movie6BufferC1Ev", buf_ctor}, {"_ZN5movie6BufferC2Ev", buf_ctor},
    {"_ZN5movie6BufferC1Em", buf_ctor_size}, {"_ZN5movie6BufferC2Em", buf_ctor_size},
    {"_ZN5movie6BufferC1EPvm", buf_ctor_data}, {"_ZN5movie6BufferC2EPvm", buf_ctor_data},
    {"_ZN5movie6BufferC1ERKS0_", buf_copy_ctor}, {"_ZN5movie6BufferC2ERKS0_", buf_copy_ctor},
    {"_ZN5movie6BufferD1Ev", buf_dtor}, {"_ZN5movie6BufferD2Ev", buf_dtor},
    {"_ZN5movie6BufferaSERKS0_", buf_assign},
    {"_ZN5movie6Buffer18SetDataAndCapacityEPvm", buf_SetDataAndCapacity}, {"_ZN5movie6Buffer11SetCapacityEm", buf_SetCapacity},
    {"_ZN5movie6Buffer8SetRangeEii", buf_SetRange}, {"_ZN5movie6Buffer4SwapERS0_", buf_Swap},
    {"_ZNK5movie6Buffer4BaseEv", buf_Base}, {"_ZNK5movie6Buffer4DataEv", buf_Data}, {"_ZNK5movie6Buffer4SizeEv", buf_Size},
    {"_ZNK5movie6Buffer6OffsetEv", buf_Offset}, {"_ZNK5movie6Buffer8CapacityEv", buf_Capacity},
    {"_ZN5movie6Buffer12SetInt32DataEi", buf_SetInt32Data}, {"_ZN5movie6Buffer12SetInt64DataEl", buf_SetInt64Data},
    {"_ZN5movie6Buffer13SetUint32DataEj", buf_SetUint32Data}, {"_ZNK5movie6Buffer12GetInt32DataEv", buf_GetInt32Data},
    {"_ZNK5movie6Buffer12GetInt64DataEv", buf_GetInt64Data}, {"_ZNK5movie6Buffer13GetUint32DataEv", buf_GetUint32Data},
    {"_ZN5movie6Buffer9SetNvnDevEPv", buf_SetNvnDev}, {"_ZNK5movie6Buffer9GetNvnDevEv", buf_GetNvnDev},
    {"_ZN5movie6Buffer10SetNvnSyncEPv", buf_SetNvnSync}, {"_ZNK5movie6Buffer10GetNvnSyncEv", buf_GetNvnSync},
    /* movie::MediaData */
    {"_ZN5movie9MediaDataC1Ev", mdata_ctor}, {"_ZN5movie9MediaDataC2Ev", mdata_ctor},
    {"_ZN5movie9MediaDataC1ERKS0_", mdata_copy_ctor}, {"_ZN5movie9MediaDataC2ERKS0_", mdata_copy_ctor},
    {"_ZN5movie9MediaDataD1Ev", mdata_dtor}, {"_ZN5movie9MediaDataD2Ev", mdata_dtor},
    {"_ZN5movie9MediaDataaSERKS0_", mdata_assign},
    {"_ZN5movie9MediaData5ClearEv", mdata_Clear},
    {"_ZN5movie9MediaData8SetInt32EPKci", mdata_SetInt32}, {"_ZN5movie9MediaData8SetInt64EPKcl", mdata_SetInt64},
    {"_ZN5movie9MediaData9SetStringEPKcS2_", mdata_SetString}, {"_ZN5movie9MediaData8SetFloatEPKcf", mdata_SetFloat},
    {"_ZN5movie9MediaData10SetPointerEPKcPv", mdata_SetPointer},
    {"_ZNK5movie9MediaData9FindInt32EPKcPi", mdata_FindInt32}, {"_ZNK5movie9MediaData9FindInt64EPKcPl", mdata_FindInt64},
    {"_ZNK5movie9MediaData10FindStringEPKcPS2_", mdata_FindString}, {"_ZNK5movie9MediaData11FindPointerEPKcPPv", mdata_FindPointer},
    {"_ZNK5movie9MediaData8ContainsEPKc", mdata_Contains}, {"_ZNK5movie9MediaData12CountEntriesEv", mdata_CountEntries},
};

void register_movie_mf_imports(void) {
    const char *env = getenv("SMO_MOVIE");
    if (env && atoi(env) == 0) { printf("movie: SMO_MOVIE=0, keeping the failing placeholder\n"); return; }
    g_log = getenv("SMO_MOVIE_LOG") != NULL;
    plat_mutex_init(&g_init_lock);
    register_imports(movie_mf_imports, (int)(sizeof movie_mf_imports / sizeof movie_mf_imports[0]));
    printf("movie: the video player is Android's (AMediaExtractor + AMediaCodec)\n");
}
