
/*
 * Experimental GVT audio RTP/Opus backend.
 *
 * This backend receives guest playback PCM from QEMU's audio mixeng and
 * pushes it into a small GStreamer appsrc pipeline:
 *   PCM S16LE 48k stereo -> opusenc -> rtpopuspay -> udpsink
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/audio.h"
#include "qom/object.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "audio_int.h"

#define TYPE_AUDIO_GVT_RTP "audio-gvt-rtp"
OBJECT_DECLARE_SIMPLE_TYPE(AudioGvtRtp, AUDIO_GVT_RTP)

struct AudioGvtRtp {
    AudioMixengBackend parent_obj;
};

typedef struct GvtRtpVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;
    GstElement *pipeline;
    GstElement *appsrc;
    bool active;
    guint64 next_pts;
    uint64_t packets;
    uint64_t bytes;
    uint64_t dropped;
    uint64_t last_packets;
    uint64_t last_bytes;
    uint64_t last_dropped;
    int64_t last_report_us;
    int freq;
    int channels;
    int bytes_per_frame;
    char *target_host;
    int target_port;
    int bitrate;
} GvtRtpVoiceOut;

static int gvt_env_int(const char *name, int defval, int minval, int maxval)
{
    const char *env = getenv(name);
    int val;

    if (!env || !*env) {
        return defval;
    }
    val = atoi(env);
    if (val < minval) {
        val = minval;
    }
    if (val > maxval) {
        val = maxval;
    }
    return val;
}

static void gvt_rtp_report(GvtRtpVoiceOut *out)
{
    int64_t now = g_get_monotonic_time();
    int64_t delta_us;
    uint64_t dpackets;
    uint64_t dbytes;
    guint64 level_bytes = 0;
    double queue_ms = 0.0;
    double kbps = 0.0;

    if (!out->last_report_us) {
        out->last_report_us = now;
        return;
    }

    delta_us = now - out->last_report_us;
    if (delta_us < 1000000) {
        return;
    }

    dpackets = out->packets - out->last_packets;
    dbytes = out->bytes - out->last_bytes;
    if (delta_us > 0) {
        kbps = (double)dbytes * 8.0 * 1000000.0 / (double)delta_us / 1000.0;
    }

    if (out->appsrc &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(out->appsrc),
                                     "current-level-bytes")) {
        g_object_get(out->appsrc, "current-level-bytes", &level_bytes, NULL);
        if (out->bytes_per_frame > 0 && out->freq > 0) {
            queue_ms = (double)level_bytes * 1000.0 /
                       (double)(out->bytes_per_frame * out->freq);
        }
    }

    error_report("gvt-audio-rtp: stats packets=%" PRIu64
                 " pps=%.1f pcm_kbps=%.1f dropped=%" PRIu64
                 " queue_ms=%.2f target=%s:%d bitrate=%d",
                 out->packets,
                 (double)dpackets * 1000000.0 / (double)delta_us,
                 kbps,
                 out->dropped,
                 queue_ms,
                 out->target_host ?: "?",
                 out->target_port,
                 out->bitrate);

    out->last_report_us = now;
    out->last_packets = out->packets;
    out->last_bytes = out->bytes;
    out->last_dropped = out->dropped;
}

static bool gvt_rtp_start_pipeline(GvtRtpVoiceOut *out)
{
    g_autofree char *desc = NULL;
    g_autoptr(GError) err = NULL;

    if (out->pipeline) {
        return true;
    }

    if (!gst_is_initialized()) {
        if (!gst_init_check(NULL, NULL, &err)) {
            error_report("gvt-audio-rtp: gst_init failed: %s",
                         err ? err->message : "unknown");
            return false;
        }
    }

    desc = g_strdup_printf(
        "appsrc name=audsrc is-live=true format=time block=false "
        "do-timestamp=false max-bytes=19200 "
        "caps=audio/x-raw,format=S16LE,rate=%d,channels=%d,layout=interleaved "
        "! queue max-size-time=30000000 max-size-bytes=0 max-size-buffers=0 leaky=downstream "
        "! opusenc bitrate=%d frame-size=10 audio-type=restricted-lowdelay inband-fec=false "
        "! rtpopuspay pt=97 ssrc=3333 "
        "! udpsink host=%s port=%d sync=false async=false",
        out->freq, out->channels, out->bitrate,
        out->target_host, out->target_port);

    out->pipeline = gst_parse_launch(desc, &err);
    if (!out->pipeline) {
        error_report("gvt-audio-rtp: pipeline create failed: %s desc=%s",
                     err ? err->message : "unknown", desc);
        return false;
    }

    out->appsrc = gst_bin_get_by_name(GST_BIN(out->pipeline), "audsrc");
    if (!out->appsrc) {
        error_report("gvt-audio-rtp: appsrc not found");
        gst_object_unref(out->pipeline);
        out->pipeline = NULL;
        return false;
    }

    if (gst_element_set_state(out->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        error_report("gvt-audio-rtp: pipeline start failed desc=%s", desc);
        gst_object_unref(out->appsrc);
        gst_object_unref(out->pipeline);
        out->appsrc = NULL;
        out->pipeline = NULL;
        return false;
    }

    error_report("gvt-audio-rtp: start target=%s:%d rate=%d channels=%d "
                 "bitrate=%d frame_ms=10",
                 out->target_host, out->target_port,
                 out->freq, out->channels, out->bitrate);
    return true;
}

static void gvt_rtp_stop_pipeline(GvtRtpVoiceOut *out)
{
    if (out->appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(out->appsrc));
    }
    if (out->pipeline) {
        gst_element_set_state(out->pipeline, GST_STATE_NULL);
    }
    g_clear_object(&out->appsrc);
    g_clear_object(&out->pipeline);
}

static int gvt_rtp_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    GvtRtpVoiceOut *out = (GvtRtpVoiceOut *)hw;
    Audiodev *dev = hw->s->dev;
    AudiodevGvtRtpOptions *opts = &dev->u.gvt_rtp;
    const char *host = opts->host ?: getenv("GVT_AUDIO_RTP_HOST");
    struct audsettings settings = {
        .freq = 48000,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = false,
    };

    out->freq = settings.freq;
    out->channels = settings.nchannels;
    out->bytes_per_frame = 4;
    out->target_host = g_strdup(host && *host ? host : "127.0.0.1");
    out->target_port = opts->has_port ? opts->port :
        gvt_env_int("GVT_AUDIO_RTP_PORT", 5006, 1, 65535);
    out->bitrate = opts->has_bitrate ? opts->bitrate :
        gvt_env_int("GVT_AUDIO_RTP_BITRATE", 96000, 16000, 510000);

    audio_pcm_init_info(&hw->info, &settings);
    hw->samples = 480;
    audio_rate_start(&out->rate);
    out->last_report_us = g_get_monotonic_time();

    return gvt_rtp_start_pipeline(out) ? 0 : -1;
}

static void gvt_rtp_fini_out(HWVoiceOut *hw)
{
    GvtRtpVoiceOut *out = (GvtRtpVoiceOut *)hw;

    gvt_rtp_stop_pipeline(out);
    g_clear_pointer(&out->target_host, g_free);
}

static size_t gvt_rtp_write_out(HWVoiceOut *hw, void *buf, size_t len)
{
    GvtRtpVoiceOut *out = (GvtRtpVoiceOut *)hw;
    int64_t bytes;
    int frames;
    GstBuffer *buffer;
    GstMapInfo map;
    GstFlowReturn flow;

    bytes = audio_rate_get_bytes(&out->rate, &hw->info, len);
    bytes -= bytes % hw->info.bytes_per_frame;
    if (bytes <= 0) {
        return 0;
    }

    if (!out->active || !out->appsrc) {
        return bytes;
    }

    buffer = gst_buffer_new_allocate(NULL, bytes, NULL);
    if (!buffer) {
        out->dropped++;
        return bytes;
    }

    gst_buffer_map(buffer, &map, GST_MAP_WRITE);
    memcpy(map.data, buf, bytes);
    gst_buffer_unmap(buffer, &map);

    frames = bytes / hw->info.bytes_per_frame;
    GST_BUFFER_PTS(buffer) = out->next_pts;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(frames, GST_SECOND,
                                                        out->freq);
    out->next_pts += GST_BUFFER_DURATION(buffer);

    flow = gst_app_src_push_buffer(GST_APP_SRC(out->appsrc), buffer);
    if (flow != GST_FLOW_OK) {
        out->dropped++;
    } else {
        out->packets++;
        out->bytes += bytes;
    }

    gvt_rtp_report(out);
    return bytes;
}

static void gvt_rtp_enable_out(HWVoiceOut *hw, bool enable)
{
    GvtRtpVoiceOut *out = (GvtRtpVoiceOut *)hw;

    out->active = enable;
    if (enable) {
        audio_rate_start(&out->rate);
        out->next_pts = 0;
    }
}

static void audio_gvt_rtp_class_init(ObjectClass *klass, const void *data)
{
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    k->max_voices_out = 1;
    k->max_voices_in = 0;
    k->voice_size_out = sizeof(GvtRtpVoiceOut);
    k->voice_size_in = 0;

    k->init_out = gvt_rtp_init_out;
    k->fini_out = gvt_rtp_fini_out;
    k->write = gvt_rtp_write_out;
    k->buffer_get_free = audio_generic_buffer_get_free;
    k->run_buffer_out = audio_generic_run_buffer_out;
    k->enable_out = gvt_rtp_enable_out;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_GVT_RTP,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioGvtRtp),
        .class_init = audio_gvt_rtp_class_init,
    }
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_GVT_RTP);
