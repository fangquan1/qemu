/*
 * Experimental GVT-g stream display backend.
 *
 * Experimental backend: attach to QEMU's GL/DMABUF display path,
 * optionally import scanout DMABUFs into EGL, capture validation frames, and
 * feed captured frames into a first-pass GStreamer encoder.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/un.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "ui/console.h"
#include "ui/dmabuf.h"
#include "ui/input.h"
#include "ui/surface.h"
#include "ui/egl-context.h"
#include "ui/egl-helpers.h"

#define GVT_STREAM_IDLE_SAMPLE_W 64
#define GVT_STREAM_IDLE_SAMPLE_H 36
#define GVT_STREAM_IDLE_SAMPLE_N \
    (GVT_STREAM_IDLE_SAMPLE_W * GVT_STREAM_IDLE_SAMPLE_H)
#define GVT_OUTPUTD_MAGIC 0x4756544f
#define GVT_OUTPUTD_VERSION 1
#define GVT_OUTPUTD_MSG_FRAME 1
#define GVT_OUTPUTD_MSG_CURSOR_POS 4
#define GVT_OUTPUTD_SOURCE_LEN 32

typedef struct GVTOutputdFrameMsg {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t stride;
    uint32_t offset;
    uint64_t modifier;
    uint64_t sequence;
    char source[GVT_OUTPUTD_SOURCE_LEN];
} GVTOutputdFrameMsg;

typedef struct GVTStreamDisplay {
    DisplayChangeListener dcl;
    QemuDmaBuf *scanout;
    uint64_t scanout_count;
    uint64_t update_count;
    uint64_t cursor_count;
    uint64_t release_count;
    int64_t last_report_ms;
    int64_t last_update_ms;
    uint64_t report_updates;
    uint64_t refresh_ms;
    uint64_t report_ms;
    uint64_t import_count;
    uint64_t import_fail_count;
    uint64_t capture_count;
    uint64_t capture_fail_count;
    uint64_t capture_ms;
    uint64_t idle_capture_ms;
    uint64_t idle_after_ms;
    uint64_t idle_probe_ms;
    uint64_t idle_changed_ppm;
    uint64_t idle_pixel_delta;
    uint64_t capture_max;
    uint64_t last_capture_checksum;
    int64_t last_capture_ms;
    int64_t last_activity_ms;
    int64_t last_content_change_ms;
    int64_t last_probe_ms;
    uint64_t last_probe_diff_ppm;
    uint64_t idle_probe_count;
    uint64_t idle_wake_count;
    bool idle_sample_valid;
    uint32_t idle_sample[GVT_STREAM_IDLE_SAMPLE_N];
    uint64_t encode_count;
    uint64_t encode_fail_count;
    uint64_t encode_max;
    GstClockTime encode_pts;
    GstClockTime encode_duration;
    int64_t last_encode_wall_ms;
    int encode_fps;
    int encode_bitrate;
    int encode_keyint;
    uint64_t rtp_port;
    uint64_t rtp_fec;
    uint64_t rtp_fec_important;
    uint64_t encode_dmabuf_count;
    uint64_t encode_cpu_count;
    GstElement *encode_pipeline;
    GstElement *encode_appsrc;
    char *capture_dir;
    char *encode_file;
    char *rtp_host;
    char *publish_socket;
    char *source_id;
    char *kms_connector;
    char *kms_device;
    DisplaySurface *capture_surface;
    GstAllocator *dmabuf_allocator;
    drmModeModeInfo kms_mode;
    drmModeCrtc *kms_old_crtc;
    QemuDmaBuf *kms_fb_dmabuf;
    QemuDmaBuf *kms_cursor_dmabuf;
    int kms_fd;
    uint32_t kms_connector_id;
    uint32_t kms_crtc_id;
    uint32_t kms_crtc_index;
    uint32_t kms_primary_plane_id;
    uint32_t kms_mode_blob_id;
    uint32_t kms_fb_id;
    uint32_t kms_conn_crtc_id_prop;
    uint32_t kms_crtc_active_prop;
    uint32_t kms_crtc_mode_id_prop;
    uint32_t kms_plane_fb_id_prop;
    uint32_t kms_plane_crtc_id_prop;
    uint32_t kms_plane_src_x_prop;
    uint32_t kms_plane_src_y_prop;
    uint32_t kms_plane_src_w_prop;
    uint32_t kms_plane_src_h_prop;
    uint32_t kms_plane_crtc_x_prop;
    uint32_t kms_plane_crtc_y_prop;
    uint32_t kms_plane_crtc_w_prop;
    uint32_t kms_plane_crtc_h_prop;
    uint32_t kms_cursor_handle;
    uint32_t kms_cursor_width;
    uint32_t kms_cursor_height;
    uint32_t kms_cursor_hot_x;
    uint32_t kms_cursor_hot_y;
    uint32_t kms_cursor_pos_x;
    uint32_t kms_cursor_pos_y;
    uint32_t kms_width;
    uint32_t kms_height;
    uint64_t kms_commit_count;
    uint64_t kms_fail_count;
    uint64_t kms_atomic_count;
    uint64_t kms_atomic_fallback_count;
    uint64_t kms_page_flip_count;
    uint64_t kms_modeset_count;
    uint64_t publish_count;
    uint64_t publish_fail_count;
    int64_t last_publish_ms;
    QemuDmaBuf *last_publish_dmabuf;
    bool kms_initialized;
    bool kms_disabled;
    bool kms_atomic;
    bool kms_atomic_ready;
    bool kms_atomic_active;
    bool encode_dmabuf;
    bool encode_dmabuf_caps_feature;
    bool encode_flip;
    egl_fb guest_fb;
    egl_fb capture_fb;
    bool verbose;
    bool import_test;
} GVTStreamDisplay;

typedef struct GVTStreamInputServer GVTStreamInputServer;

typedef struct GVTStreamInputClient {
    GVTStreamInputServer *server;
    int fd;
    GString *buffer;
} GVTStreamInputClient;

struct GVTStreamInputServer {
    int listen_fd;
    GList *clients;
    uint64_t connected;
    uint64_t messages;
    uint64_t events;
    uint64_t parse_errors;
};

static const DisplayChangeListenerOps gvt_stream_ops;
static GVTStreamInputServer *gvt_stream_input_server;
static GVTStreamDisplay *gvt_stream_input_display;
static int64_t gvt_stream_last_input_ms;

static uint64_t gvt_stream_getenv_u64(const char *name,
                                      uint64_t defval,
                                      uint64_t minval,
                                      uint64_t maxval)
{
    const char *env = g_getenv(name);
    uint64_t val;
    char *end = NULL;

    if (!env || !*env) {
        return defval;
    }

    errno = 0;
    val = g_ascii_strtoull(env, &end, 0);
    if (errno || end == env || (end && *end)) {
        warn_report("gvt-stream: ignoring invalid %s=%s", name, env);
        return defval;
    }
    if (val < minval) {
        return minval;
    }
    if (val > maxval) {
        return maxval;
    }
    return val;
}

static bool gvt_stream_getenv_bool(const char *name, bool defval)
{
    const char *env = g_getenv(name);

    if (!env || !*env) {
        return defval;
    }
    if (!g_ascii_strcasecmp(env, "1") ||
        !g_ascii_strcasecmp(env, "on") ||
        !g_ascii_strcasecmp(env, "yes") ||
        !g_ascii_strcasecmp(env, "true")) {
        return true;
    }
    if (!g_ascii_strcasecmp(env, "0") ||
        !g_ascii_strcasecmp(env, "off") ||
        !g_ascii_strcasecmp(env, "no") ||
        !g_ascii_strcasecmp(env, "false")) {
        return false;
    }

    warn_report("gvt-stream: ignoring invalid %s=%s", name, env);
    return defval;
}

static uint64_t gvt_stream_effective_capture_ms(GVTStreamDisplay *gdpy,
                                                int64_t now_ms)
{
    int64_t last_activity_ms = gdpy->last_activity_ms;

    if (gdpy->last_content_change_ms > last_activity_ms) {
        last_activity_ms = gdpy->last_content_change_ms;
    }
    if (gvt_stream_last_input_ms > last_activity_ms) {
        last_activity_ms = gvt_stream_last_input_ms;
    }

    if (gdpy->idle_capture_ms <= gdpy->capture_ms ||
        !gdpy->idle_after_ms || !last_activity_ms) {
        return gdpy->capture_ms;
    }
    if (now_ms - last_activity_ms <= gdpy->idle_after_ms) {
        return gdpy->capture_ms;
    }
    return gdpy->idle_capture_ms;
}

static int gvt_stream_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int64_t gvt_stream_qdict_get_clamped_int(QDict *dict,
                                                const char *key,
                                                int64_t min,
                                                int64_t max,
                                                int64_t defval)
{
    int64_t val = qdict_get_try_int(dict, key, defval);

    if (val < min) {
        return min;
    }
    if (val > max) {
        return max;
    }
    return val;
}

static bool gvt_stream_parse_qcode(const char *name, QKeyCode *qcode)
{
    int value;

    if (!name || !*name) {
        return false;
    }
    value = qapi_enum_parse(&QKeyCode_lookup, name, -1, NULL);
    if (value < 0 || value >= Q_KEY_CODE__MAX) {
        return false;
    }
    *qcode = value;
    return true;
}

static bool gvt_stream_parse_button(const char *name, InputButton *button)
{
    int value;

    if (!name || !*name) {
        return false;
    }
    value = qapi_enum_parse(&InputButton_lookup, name, -1, NULL);
    if (value < 0 || value >= INPUT_BUTTON__MAX) {
        return false;
    }
    *button = value;
    return true;
}

static void gvt_stream_publish_dmabuf(GVTStreamDisplay *gdpy,
                                      QemuDmaBuf *dmabuf,
                                      int64_t now_ms,
                                      bool force)
{
    struct sockaddr_un addr = { 0 };
    GVTOutputdFrameMsg msg = { 0 };
    struct msghdr msgh = { 0 };
    struct iovec iov = {
        .iov_base = &msg,
        .iov_len = sizeof(msg),
    };
    char control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;
    const int *fds;
    const uint32_t *strides;
    const uint32_t *offsets;
    int n_fds = 0;
    int n_strides = 0;
    int n_offsets = 0;
    int fd = -1;
    int sock = -1;

    if (!gdpy->publish_socket || !*gdpy->publish_socket || !dmabuf) {
        return;
    }
    if (!force && gdpy->last_publish_dmabuf == dmabuf &&
        now_ms - gdpy->last_publish_ms < 1000) {
        return;
    }
    if (qemu_dmabuf_get_num_planes(dmabuf) != 1) {
        gdpy->publish_fail_count++;
        if (gdpy->verbose || gdpy->publish_fail_count <= 4) {
            warn_report("gvt-stream-publish: only single-plane dmabuf is "
                        "supported for now");
        }
        return;
    }

    fds = qemu_dmabuf_get_fds(dmabuf, &n_fds);
    strides = qemu_dmabuf_get_strides(dmabuf, &n_strides);
    offsets = qemu_dmabuf_get_offsets(dmabuf, &n_offsets);
    if (n_fds < 1 || fds[0] < 0 || n_strides < 1 || n_offsets < 1) {
        gdpy->publish_fail_count++;
        warn_report("gvt-stream-publish: incomplete dmabuf metadata");
        return;
    }

    fd = fds[0];
    sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        gdpy->publish_fail_count++;
        warn_report("gvt-stream-publish: socket failed: %s", strerror(errno));
        return;
    }

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", gdpy->publish_socket);

    msg.magic = GVT_OUTPUTD_MAGIC;
    msg.version = GVT_OUTPUTD_VERSION;
    msg.type = GVT_OUTPUTD_MSG_FRAME;
    msg.width = qemu_dmabuf_get_width(dmabuf);
    msg.height = qemu_dmabuf_get_height(dmabuf);
    msg.fourcc = qemu_dmabuf_get_fourcc(dmabuf);
    msg.stride = strides[0];
    msg.offset = offsets[0];
    msg.modifier = qemu_dmabuf_get_modifier(dmabuf);
    msg.sequence = gdpy->publish_count + 1;
    snprintf(msg.source, sizeof(msg.source), "%s", gdpy->source_id ?: "vm");

    memset(control, 0, sizeof(control));
    msgh.msg_name = &addr;
    msgh.msg_namelen = sizeof(addr);
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = control;
    msgh.msg_controllen = sizeof(control);

    cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    if (sendmsg(sock, &msgh, MSG_NOSIGNAL) < 0) {
        gdpy->publish_fail_count++;
        if (gdpy->verbose || gdpy->publish_fail_count <= 8) {
            warn_report("gvt-stream-publish: sendmsg %s failed: %s",
                        gdpy->publish_socket, strerror(errno));
        }
        close(sock);
        return;
    }

    close(sock);
    gdpy->publish_count++;
    gdpy->last_publish_ms = now_ms;
    gdpy->last_publish_dmabuf = dmabuf;

    if (gdpy->verbose || gdpy->publish_count <= 8 ||
        gdpy->publish_count % 60 == 0) {
        error_report("gvt-stream-publish: sent #%" PRIu64
                     " source=%s size=%ux%u fourcc=0x%08x stride=%u",
                     gdpy->publish_count, gdpy->source_id ?: "vm",
                     msg.width, msg.height, msg.fourcc, msg.stride);
    }
}

static void gvt_stream_publish_cursor_pos(uint32_t x, uint32_t y)
{
    GVTOutputdFrameMsg msg = { 0 };
    struct sockaddr_un addr = { 0 };
    struct msghdr msgh = { 0 };
    struct iovec iov = {
        .iov_base = &msg,
        .iov_len = sizeof(msg),
    };
    GVTStreamDisplay *gdpy = gvt_stream_input_display;
    int sock;

    if (!gdpy || !gdpy->publish_socket || !*gdpy->publish_socket) {
        return;
    }

    sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        gdpy->publish_fail_count++;
        return;
    }

    msg.magic = GVT_OUTPUTD_MAGIC;
    msg.version = GVT_OUTPUTD_VERSION;
    msg.type = GVT_OUTPUTD_MSG_CURSOR_POS;
    msg.width = x;
    msg.height = y;
    snprintf(msg.source, sizeof(msg.source), "%s", gdpy->source_id ?: "vm");

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", gdpy->publish_socket);
    msgh.msg_name = &addr;
    msgh.msg_namelen = sizeof(addr);
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;

    if (sendmsg(sock, &msgh, MSG_NOSIGNAL) < 0) {
        gdpy->publish_fail_count++;
    }
    close(sock);
}

static void gvt_stream_input_send_qcode(const char *name, bool down,
                                        GVTStreamInputServer *server)
{
    QKeyCode qcode;

    if (!gvt_stream_parse_qcode(name, &qcode)) {
        server->parse_errors++;
        warn_report("gvt-stream-input: ignoring unknown qcode=%s",
                    name ?: "");
        return;
    }
    qemu_input_event_send_key_qcode(NULL, qcode, down);
    server->events++;
}

static void gvt_stream_input_process_dict(GVTStreamInputServer *server,
                                          QDict *dict);

static void gvt_stream_input_process_batch(GVTStreamInputServer *server,
                                           QDict *dict)
{
    QList *items = qdict_get_qlist(dict, "items");
    const QListEntry *entry;

    if (!items) {
        server->parse_errors++;
        return;
    }

    QLIST_FOREACH_ENTRY(items, entry) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));

        if (item) {
            gvt_stream_input_process_dict(server, item);
        } else {
            server->parse_errors++;
        }
    }
}

static void gvt_stream_input_process_combo(GVTStreamInputServer *server,
                                           QDict *dict)
{
    QList *qcodes = qdict_get_qlist(dict, "qcodes");
    const QListEntry *entry;
    GArray *parsed;
    int i;

    if (!qcodes) {
        server->parse_errors++;
        return;
    }

    parsed = g_array_new(FALSE, FALSE, sizeof(QKeyCode));
    QLIST_FOREACH_ENTRY(qcodes, entry) {
        QString *qstr = qobject_to(QString, qlist_entry_obj(entry));
        QKeyCode qcode;

        if (!qstr ||
            !gvt_stream_parse_qcode(qstring_get_str(qstr), &qcode)) {
            server->parse_errors++;
            continue;
        }
        g_array_append_val(parsed, qcode);
    }

    for (i = 0; i < parsed->len; i++) {
        QKeyCode qcode = g_array_index(parsed, QKeyCode, i);
        qemu_input_event_send_key_qcode(NULL, qcode, true);
        server->events++;
    }
    for (i = parsed->len - 1; i >= 0; i--) {
        QKeyCode qcode = g_array_index(parsed, QKeyCode, i);
        qemu_input_event_send_key_qcode(NULL, qcode, false);
        server->events++;
    }
    g_array_free(parsed, TRUE);
}

static void gvt_stream_input_process_dict(GVTStreamInputServer *server,
                                          QDict *dict)
{
    const char *type = qdict_get_try_str(dict, "type");

    if (!type) {
        server->parse_errors++;
        return;
    }

    if (!g_strcmp0(type, "batch")) {
        gvt_stream_input_process_batch(server, dict);
        return;
    }
    if (!g_strcmp0(type, "move")) {
        int x = gvt_stream_qdict_get_clamped_int(dict, "x", 0, 0x7fff, 0);
        int y = gvt_stream_qdict_get_clamped_int(dict, "y", 0, 0x7fff, 0);

        qemu_input_queue_abs(NULL, INPUT_AXIS_X, x, 0, 0x7fff);
        qemu_input_queue_abs(NULL, INPUT_AXIS_Y, y, 0, 0x7fff);
        gvt_stream_publish_cursor_pos((uint32_t)x, (uint32_t)y);
        server->events += 2;
        return;
    }
    if (!g_strcmp0(type, "button")) {
        const char *name = qdict_get_try_str(dict, "button") ?: "left";
        bool down = qdict_get_try_bool(dict, "down", false);
        InputButton button;

        if (!gvt_stream_parse_button(name, &button)) {
            server->parse_errors++;
            warn_report("gvt-stream-input: ignoring unknown button=%s", name);
            return;
        }
        qemu_input_queue_btn(NULL, button, down);
        server->events++;
        return;
    }
    if (!g_strcmp0(type, "wheel")) {
        int64_t delta = qdict_get_try_int(dict, "delta", 0);
        InputButton button = delta > 0 ? INPUT_BUTTON_WHEEL_UP :
                                        INPUT_BUTTON_WHEEL_DOWN;

        qemu_input_queue_btn(NULL, button, true);
        qemu_input_queue_btn(NULL, button, false);
        server->events += 2;
        return;
    }
    if (!g_strcmp0(type, "key")) {
        gvt_stream_input_send_qcode(qdict_get_try_str(dict, "qcode"),
                                    qdict_get_try_bool(dict, "down", false),
                                    server);
        return;
    }
    if (!g_strcmp0(type, "combo")) {
        gvt_stream_input_process_combo(server, dict);
        return;
    }

    server->parse_errors++;
}

static void gvt_stream_input_process_line(GVTStreamInputClient *client,
                                          const char *line)
{
    GVTStreamInputServer *server = client->server;
    Error *err = NULL;
    QObject *obj;
    QDict *dict;

    if (!line || !*line) {
        return;
    }

    obj = qobject_from_json(line, &err);
    if (err) {
        server->parse_errors++;
        warn_report("gvt-stream-input: json parse failed: %s",
                    error_get_pretty(err));
        error_free(err);
        return;
    }
    dict = qobject_to(QDict, obj);
    if (!dict) {
        server->parse_errors++;
        qobject_unref(obj);
        return;
    }

    gvt_stream_last_input_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    gvt_stream_input_process_dict(server, dict);
    qemu_input_event_sync();
    server->messages++;
    qobject_unref(obj);
}

static void gvt_stream_input_client_close(GVTStreamInputClient *client)
{
    if (!client) {
        return;
    }
    qemu_set_fd_handler(client->fd, NULL, NULL, NULL);
    close(client->fd);
    client->server->clients = g_list_remove(client->server->clients, client);
    g_string_free(client->buffer, TRUE);
    g_free(client);
}

static void gvt_stream_input_client_read(void *opaque)
{
    GVTStreamInputClient *client = opaque;
    char tmp[4096];

    for (;;) {
        ssize_t ret = read(client->fd, tmp, sizeof(tmp));

        if (ret > 0) {
            char *nl;

            g_string_append_len(client->buffer, tmp, ret);
            while ((nl = strchr(client->buffer->str, '\n'))) {
                g_autofree char *line =
                    g_strndup(client->buffer->str, nl - client->buffer->str);
                g_string_erase(client->buffer, 0,
                               nl - client->buffer->str + 1);
                gvt_stream_input_process_line(client, line);
            }
            if (client->buffer->len > 1024 * 1024) {
                client->server->parse_errors++;
                warn_report("gvt-stream-input: closing oversized client buffer");
                gvt_stream_input_client_close(client);
                return;
            }
            continue;
        }
        if (ret == 0) {
            gvt_stream_input_client_close(client);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        gvt_stream_input_client_close(client);
        return;
    }
}

static void gvt_stream_input_accept(void *opaque)
{
    GVTStreamInputServer *server = opaque;

    for (;;) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd = accept(server->listen_fd, (struct sockaddr *)&addr, &addrlen);
        GVTStreamInputClient *client;
        int one = 1;

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                warn_report("gvt-stream-input: accept failed: %s",
                            strerror(errno));
            }
            return;
        }

        qemu_set_cloexec(fd);
        gvt_stream_set_nonblock(fd);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        client = g_new0(GVTStreamInputClient, 1);
        client->server = server;
        client->fd = fd;
        client->buffer = g_string_new(NULL);
        server->clients = g_list_prepend(server->clients, client);
        server->connected++;
        qemu_set_fd_handler(fd, gvt_stream_input_client_read, NULL, client);
        error_report("gvt-stream-input: client connected from %s fd=%d total=%" PRIu64,
                     inet_ntoa(addr.sin_addr), fd, server->connected);
    }
}

static void gvt_stream_input_start(void)
{
    const char *host = g_getenv("GVT_STREAM_INPUT_HOST") ?: "0.0.0.0";
    uint64_t port = gvt_stream_getenv_u64("GVT_STREAM_INPUT_PORT",
                                          0, 0, 65535);
    struct sockaddr_in addr = { 0 };
    int fd;

    if (!port || gvt_stream_input_server) {
        return;
    }

    fd = qemu_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        warn_report("gvt-stream-input: socket failed: %s", strerror(errno));
        return;
    }

    socket_set_fast_reuse(fd);
    qemu_set_cloexec(fd);
    gvt_stream_set_nonblock(fd);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        warn_report("gvt-stream-input: invalid listen host %s", host);
        close(fd);
        return;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        warn_report("gvt-stream-input: bind %s:%" PRIu64 " failed: %s",
                    host, port, strerror(errno));
        close(fd);
        return;
    }
    if (listen(fd, 4) < 0) {
        warn_report("gvt-stream-input: listen failed: %s", strerror(errno));
        close(fd);
        return;
    }

    gvt_stream_input_server = g_new0(GVTStreamInputServer, 1);
    gvt_stream_input_server->listen_fd = fd;
    qemu_set_fd_handler(fd, gvt_stream_input_accept, NULL,
                        gvt_stream_input_server);
    error_report("gvt-stream-input: listening on %s:%" PRIu64, host, port);
}

static void gvt_stream_fourcc_to_str(uint32_t fourcc, char out[5])
{
    int i;

    out[0] = fourcc & 0xff;
    out[1] = (fourcc >> 8) & 0xff;
    out[2] = (fourcc >> 16) & 0xff;
    out[3] = (fourcc >> 24) & 0xff;
    out[4] = 0;

    for (i = 0; i < 4; i++) {
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) {
            out[i] = '.';
        }
    }
}

static void gvt_stream_log_dmabuf(GVTStreamDisplay *gdpy,
                                  const char *event,
                                  QemuDmaBuf *dmabuf)
{
    const uint32_t *offsets;
    const uint32_t *strides;
    const int *fds;
    int n_offsets = 0;
    int n_strides = 0;
    int n_fds = 0;
    uint32_t planes;
    char fourcc[5];
    GString *fd_buf;
    GString *stride_buf;
    GString *offset_buf;
    int i;

    if (!dmabuf) {
        error_report("gvt-stream: %s console=%d dmabuf=NULL",
                     event, qemu_console_get_index(gdpy->dcl.con));
        return;
    }

    planes = qemu_dmabuf_get_num_planes(dmabuf);
    fds = qemu_dmabuf_get_fds(dmabuf, &n_fds);
    offsets = qemu_dmabuf_get_offsets(dmabuf, &n_offsets);
    strides = qemu_dmabuf_get_strides(dmabuf, &n_strides);
    gvt_stream_fourcc_to_str(qemu_dmabuf_get_fourcc(dmabuf), fourcc);

    fd_buf = g_string_new(NULL);
    stride_buf = g_string_new(NULL);
    offset_buf = g_string_new(NULL);
    for (i = 0; i < planes; i++) {
        g_string_append_printf(fd_buf, "%s%d", i ? "," : "",
                               i < n_fds ? fds[i] : -1);
        g_string_append_printf(stride_buf, "%s%u", i ? "," : "",
                               i < n_strides ? strides[i] : 0);
        g_string_append_printf(offset_buf, "%s%u", i ? "," : "",
                               i < n_offsets ? offsets[i] : 0);
    }

    error_report("gvt-stream: %s #%" PRIu64 " console=%d dmabuf=%p "
                 "fds=[%s] planes=%u size=%ux%u backing=%ux%u "
                 "xy=%u,%u strides=[%s] offsets=[%s] fourcc=%s/0x%08x "
                 "modifier=0x%016" PRIx64 " y0_top=%d allow_fences=%d "
                 "fence_fd=%d sync=%p draw_submitted=%d",
                 event, gdpy->scanout_count,
                 qemu_console_get_index(gdpy->dcl.con), dmabuf,
                 fd_buf->str, planes,
                 qemu_dmabuf_get_width(dmabuf),
                 qemu_dmabuf_get_height(dmabuf),
                 qemu_dmabuf_get_backing_width(dmabuf),
                 qemu_dmabuf_get_backing_height(dmabuf),
                 qemu_dmabuf_get_x(dmabuf), qemu_dmabuf_get_y(dmabuf),
                 stride_buf->str, offset_buf->str, fourcc,
                 qemu_dmabuf_get_fourcc(dmabuf),
                 qemu_dmabuf_get_modifier(dmabuf),
                 qemu_dmabuf_get_y0_top(dmabuf),
                 qemu_dmabuf_get_allow_fences(dmabuf),
                 qemu_dmabuf_get_fence_fd(dmabuf),
                 qemu_dmabuf_get_sync(dmabuf),
                 qemu_dmabuf_get_draw_submitted(dmabuf));

    g_string_free(fd_buf, TRUE);
    g_string_free(stride_buf, TRUE);
    g_string_free(offset_buf, TRUE);
}

static const char *gvt_stream_kms_connector_type_name(uint32_t type)
{
    switch (type) {
    case DRM_MODE_CONNECTOR_VGA: return "VGA";
    case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
    case DRM_MODE_CONNECTOR_eDP: return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
    case DRM_MODE_CONNECTOR_DSI: return "DSI";
    default: return "Unknown";
    }
}

static void gvt_stream_kms_connector_name(drmModeConnector *conn,
                                          char *out, size_t out_len)
{
    snprintf(out, out_len, "%s-%u",
             gvt_stream_kms_connector_type_name(conn->connector_type),
             conn->connector_type_id);
}

static drmModeConnector *gvt_stream_kms_find_connector(int fd, drmModeRes *res,
                                                       const char *wanted)
{
    int i;

    for (i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        char name[64];

        if (!conn) {
            continue;
        }
        gvt_stream_kms_connector_name(conn, name, sizeof(name));
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 &&
            (!wanted || !*wanted || !g_strcmp0(name, wanted))) {
            return conn;
        }
        drmModeFreeConnector(conn);
    }

    return NULL;
}

static bool gvt_stream_kms_pick_mode(drmModeConnector *conn,
                                     uint32_t width, uint32_t height,
                                     drmModeModeInfo *mode)
{
    int i;

    for (i = 0; i < conn->count_modes; i++) {
        if (conn->modes[i].hdisplay == width &&
            conn->modes[i].vdisplay == height) {
            *mode = conn->modes[i];
            return true;
        }
    }
    return false;
}

static uint32_t gvt_stream_kms_pick_crtc(int fd, drmModeRes *res,
                                         drmModeConnector *conn)
{
    int i, j;

    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
        uint32_t crtc_id = 0;

        if (enc) {
            crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
        if (crtc_id) {
            return crtc_id;
        }
    }

    for (i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);

        if (!enc) {
            continue;
        }
        for (j = 0; j < res->count_crtcs; j++) {
            if (enc->possible_crtcs & (1 << j)) {
                uint32_t crtc_id = res->crtcs[j];

                drmModeFreeEncoder(enc);
                return crtc_id;
            }
        }
        drmModeFreeEncoder(enc);
    }

    return 0;
}

static uint32_t gvt_stream_kms_crtc_index(drmModeRes *res, uint32_t crtc_id)
{
    int i;

    for (i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) {
            return i;
        }
    }
    return UINT32_MAX;
}

static uint32_t gvt_stream_kms_get_prop(int fd, uint32_t object_id,
                                        uint32_t object_type,
                                        const char *name,
                                        uint64_t *value)
{
    drmModeObjectProperties *props;
    uint32_t prop_id = 0;
    int i;

    props = drmModeObjectGetProperties(fd, object_id, object_type);
    if (!props) {
        return 0;
    }

    for (i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);

        if (!prop) {
            continue;
        }
        if (!strcmp(prop->name, name)) {
            prop_id = prop->prop_id;
            if (value) {
                *value = props->prop_values[i];
            }
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    return prop_id;
}

static bool gvt_stream_kms_plane_supports_format(drmModePlane *plane,
                                                 uint32_t fourcc)
{
    uint32_t i;

    for (i = 0; i < plane->count_formats; i++) {
        if (plane->formats[i] == fourcc) {
            return true;
        }
    }
    return false;
}

static uint32_t gvt_stream_kms_find_primary_plane(GVTStreamDisplay *gdpy,
                                                  QemuDmaBuf *dmabuf)
{
    drmModePlaneRes *planes;
    uint32_t fourcc = qemu_dmabuf_get_fourcc(dmabuf);
    uint32_t plane_id = 0;
    uint32_t i;

    planes = drmModeGetPlaneResources(gdpy->kms_fd);
    if (!planes) {
        warn_report("gvt-stream-kms: drmModeGetPlaneResources failed: %s",
                    strerror(errno));
        return 0;
    }

    for (i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(gdpy->kms_fd,
                                              planes->planes[i]);
        uint64_t type = 0;

        if (!plane) {
            continue;
        }
        if (!(plane->possible_crtcs & (1u << gdpy->kms_crtc_index))) {
            drmModeFreePlane(plane);
            continue;
        }
        if (!gvt_stream_kms_plane_supports_format(plane, fourcc)) {
            drmModeFreePlane(plane);
            continue;
        }

        gvt_stream_kms_get_prop(gdpy->kms_fd, plane->plane_id,
                                DRM_MODE_OBJECT_PLANE, "type", &type);
        if (type == DRM_PLANE_TYPE_PRIMARY) {
            plane_id = plane->plane_id;
            drmModeFreePlane(plane);
            break;
        }
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planes);
    return plane_id;
}

static bool gvt_stream_kms_setup_atomic(GVTStreamDisplay *gdpy,
                                        QemuDmaBuf *dmabuf)
{
    if (!gdpy->kms_atomic) {
        return false;
    }

    if (drmSetClientCap(gdpy->kms_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES,
                        1) != 0) {
        warn_report("gvt-stream-kms: UNIVERSAL_PLANES cap failed: %s",
                    strerror(errno));
        return false;
    }
    if (drmSetClientCap(gdpy->kms_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        warn_report("gvt-stream-kms: ATOMIC cap failed: %s", strerror(errno));
        return false;
    }

    gdpy->kms_conn_crtc_id_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_connector_id,
                                DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    gdpy->kms_crtc_active_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_crtc_id,
                                DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
    gdpy->kms_crtc_mode_id_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_crtc_id,
                                DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);

    gdpy->kms_primary_plane_id = gvt_stream_kms_find_primary_plane(gdpy,
                                                                   dmabuf);
    if (!gdpy->kms_primary_plane_id) {
        warn_report("gvt-stream-kms: no primary plane for atomic commit");
        return false;
    }

    gdpy->kms_plane_fb_id_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_primary_plane_id,
                                DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
    gdpy->kms_plane_crtc_id_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_primary_plane_id,
                                DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
    gdpy->kms_plane_src_x_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_primary_plane_id,
                                DRM_MODE_OBJECT_PLANE, "SRC_X", NULL);
    gdpy->kms_plane_src_y_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_primary_plane_id,
                                DRM_MODE_OBJECT_PLANE, "SRC_Y", NULL);
    gdpy->kms_plane_src_w_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_primary_plane_id,
                                DRM_MODE_OBJECT_PLANE, "SRC_W", NULL);
    gdpy->kms_plane_src_h_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_primary_plane_id,
                                DRM_MODE_OBJECT_PLANE, "SRC_H", NULL);
    gdpy->kms_plane_crtc_x_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_primary_plane_id,
                                DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL);
    gdpy->kms_plane_crtc_y_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_primary_plane_id,
                                DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL);
    gdpy->kms_plane_crtc_w_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_primary_plane_id,
                                DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL);
    gdpy->kms_plane_crtc_h_prop =
        gvt_stream_kms_get_prop(gdpy->kms_fd, gdpy->kms_primary_plane_id,
                                DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL);

    if (!gdpy->kms_conn_crtc_id_prop ||
        !gdpy->kms_crtc_active_prop ||
        !gdpy->kms_crtc_mode_id_prop ||
        !gdpy->kms_plane_fb_id_prop ||
        !gdpy->kms_plane_crtc_id_prop ||
        !gdpy->kms_plane_src_x_prop ||
        !gdpy->kms_plane_src_y_prop ||
        !gdpy->kms_plane_src_w_prop ||
        !gdpy->kms_plane_src_h_prop ||
        !gdpy->kms_plane_crtc_x_prop ||
        !gdpy->kms_plane_crtc_y_prop ||
        !gdpy->kms_plane_crtc_w_prop ||
        !gdpy->kms_plane_crtc_h_prop) {
        warn_report("gvt-stream-kms: missing atomic properties");
        return false;
    }

    if (drmModeCreatePropertyBlob(gdpy->kms_fd, &gdpy->kms_mode,
                                  sizeof(gdpy->kms_mode),
                                  &gdpy->kms_mode_blob_id) != 0) {
        warn_report("gvt-stream-kms: create mode blob failed: %s",
                    strerror(errno));
        gdpy->kms_mode_blob_id = 0;
        return false;
    }

    error_report("gvt-stream-kms: atomic-ready crtc=%u connector=%u "
                 "primary_plane=%u mode_blob=%u",
                 gdpy->kms_crtc_id, gdpy->kms_connector_id,
                 gdpy->kms_primary_plane_id, gdpy->kms_mode_blob_id);
    return true;
}

static void gvt_stream_kms_remove_fb(GVTStreamDisplay *gdpy)
{
    if (gdpy->kms_fd >= 0 && gdpy->kms_fb_id) {
        drmModeRmFB(gdpy->kms_fd, gdpy->kms_fb_id);
    }
    gdpy->kms_fb_id = 0;
    gdpy->kms_fb_dmabuf = NULL;
}

typedef struct GVTStreamKmsFlipWait {
    bool done;
} GVTStreamKmsFlipWait;

static void gvt_stream_kms_page_flip_handler(int fd, unsigned int frame,
                                             unsigned int sec,
                                             unsigned int usec, void *data)
{
    GVTStreamKmsFlipWait *wait = data;

    (void)fd;
    (void)frame;
    (void)sec;
    (void)usec;
    wait->done = true;
}

static bool gvt_stream_kms_wait_page_flip(GVTStreamDisplay *gdpy,
                                          GVTStreamKmsFlipWait *wait)
{
    drmEventContext ev = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler = gvt_stream_kms_page_flip_handler,
    };
    int64_t deadline_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100;

    while (!wait->done) {
        int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        int timeout_ms = (int)(deadline_ms - now_ms);
        struct pollfd pfd = {
            .fd = gdpy->kms_fd,
            .events = POLLIN,
        };
        int ret;

        if (timeout_ms <= 0) {
            gdpy->kms_fail_count++;
            warn_report("gvt-stream-kms: PageFlip event timeout");
            return false;
        }

        ret = poll(&pfd, 1, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            gdpy->kms_fail_count++;
            warn_report("gvt-stream-kms: PageFlip poll failed: %s",
                        strerror(errno));
            return false;
        }
        if (ret == 0) {
            continue;
        }
        if (pfd.revents & POLLIN) {
            if (drmHandleEvent(gdpy->kms_fd, &ev) != 0) {
                gdpy->kms_fail_count++;
                warn_report("gvt-stream-kms: drmHandleEvent failed: %s",
                            strerror(errno));
                return false;
            }
        } else if (pfd.revents) {
            gdpy->kms_fail_count++;
            warn_report("gvt-stream-kms: PageFlip poll revents=0x%x",
                        pfd.revents);
            return false;
        }
    }

    return true;
}

static bool gvt_stream_kms_atomic_add(drmModeAtomicReq *req, uint32_t object_id,
                                      uint32_t prop_id, uint64_t value)
{
    return drmModeAtomicAddProperty(req, object_id, prop_id, value) >= 0;
}

static bool gvt_stream_kms_atomic_commit(GVTStreamDisplay *gdpy,
                                         uint32_t fb_id)
{
    drmModeAtomicReq *req;
    uint32_t width = gdpy->kms_width;
    uint32_t height = gdpy->kms_height;
    uint32_t flags = 0;
    bool modeset = !gdpy->kms_atomic_active;
    bool ok = false;

    if (!gdpy->kms_atomic_ready || !gdpy->kms_primary_plane_id) {
        return false;
    }

    req = drmModeAtomicAlloc();
    if (!req) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: drmModeAtomicAlloc failed");
        return false;
    }

    if (modeset) {
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
        ok = gvt_stream_kms_atomic_add(req, gdpy->kms_connector_id,
                                       gdpy->kms_conn_crtc_id_prop,
                                       gdpy->kms_crtc_id) &&
             gvt_stream_kms_atomic_add(req, gdpy->kms_crtc_id,
                                       gdpy->kms_crtc_active_prop, 1) &&
             gvt_stream_kms_atomic_add(req, gdpy->kms_crtc_id,
                                       gdpy->kms_crtc_mode_id_prop,
                                       gdpy->kms_mode_blob_id);
        if (!ok) {
            warn_report("gvt-stream-kms: atomic modeset property add failed");
            goto out;
        }
    }

    ok = gvt_stream_kms_atomic_add(req, gdpy->kms_primary_plane_id,
                                   gdpy->kms_plane_fb_id_prop, fb_id) &&
         gvt_stream_kms_atomic_add(req, gdpy->kms_primary_plane_id,
                                   gdpy->kms_plane_crtc_id_prop,
                                   gdpy->kms_crtc_id) &&
         gvt_stream_kms_atomic_add(req, gdpy->kms_primary_plane_id,
                                   gdpy->kms_plane_src_x_prop, 0) &&
         gvt_stream_kms_atomic_add(req, gdpy->kms_primary_plane_id,
                                   gdpy->kms_plane_src_y_prop, 0) &&
         gvt_stream_kms_atomic_add(req, gdpy->kms_primary_plane_id,
                                   gdpy->kms_plane_src_w_prop,
                                   (uint64_t)width << 16) &&
         gvt_stream_kms_atomic_add(req, gdpy->kms_primary_plane_id,
                                   gdpy->kms_plane_src_h_prop,
                                   (uint64_t)height << 16) &&
         gvt_stream_kms_atomic_add(req, gdpy->kms_primary_plane_id,
                                   gdpy->kms_plane_crtc_x_prop, 0) &&
         gvt_stream_kms_atomic_add(req, gdpy->kms_primary_plane_id,
                                   gdpy->kms_plane_crtc_y_prop, 0) &&
         gvt_stream_kms_atomic_add(req, gdpy->kms_primary_plane_id,
                                   gdpy->kms_plane_crtc_w_prop, width) &&
         gvt_stream_kms_atomic_add(req, gdpy->kms_primary_plane_id,
                                   gdpy->kms_plane_crtc_h_prop, height);
    if (!ok) {
        warn_report("gvt-stream-kms: atomic plane property add failed");
        goto out;
    }

    if (drmModeAtomicCommit(gdpy->kms_fd, req, flags, NULL) != 0) {
        gdpy->kms_fail_count++;
        gdpy->kms_atomic_fallback_count++;
        warn_report("gvt-stream-kms: AtomicCommit failed, fallback to "
                    "legacy KMS: %s", strerror(errno));
        gdpy->kms_atomic_ready = false;
        ok = false;
        goto out;
    }

    gdpy->kms_atomic_active = true;
    ok = true;

out:
    drmModeAtomicFree(req);
    return ok;
}

static void gvt_stream_kms_clear_cursor(GVTStreamDisplay *gdpy)
{
    if (gdpy->kms_fd >= 0 && gdpy->kms_crtc_id) {
        drmModeSetCursor(gdpy->kms_fd, gdpy->kms_crtc_id, 0, 0, 0);
    }
    if (gdpy->kms_fd >= 0 && gdpy->kms_cursor_handle) {
        drmCloseBufferHandle(gdpy->kms_fd, gdpy->kms_cursor_handle);
    }
    gdpy->kms_cursor_handle = 0;
    gdpy->kms_cursor_dmabuf = NULL;
    gdpy->kms_cursor_width = 0;
    gdpy->kms_cursor_height = 0;
}

static void gvt_stream_kms_move_cursor(GVTStreamDisplay *gdpy)
{
    if (!gdpy->kms_initialized || gdpy->kms_fd < 0 ||
        !gdpy->kms_cursor_handle) {
        return;
    }
    if (drmModeMoveCursor(gdpy->kms_fd, gdpy->kms_crtc_id,
                          (int)gdpy->kms_cursor_pos_x,
                          (int)gdpy->kms_cursor_pos_y) != 0) {
        gdpy->kms_fail_count++;
        if (gdpy->verbose) {
            warn_report("gvt-stream-kms: MoveCursor failed: %s",
                        strerror(errno));
        }
    }
}

static void gvt_stream_kms_update_cursor(GVTStreamDisplay *gdpy)
{
    QemuDmaBuf *dmabuf = gdpy->kms_cursor_dmabuf;
    const int *fds;
    int n_fds = 0;
    uint32_t handle = 0;
    uint32_t width;
    uint32_t height;

    if (!gdpy->kms_initialized || gdpy->kms_fd < 0 || !dmabuf) {
        return;
    }
    if (gdpy->kms_cursor_handle && gdpy->kms_cursor_dmabuf == dmabuf) {
        gvt_stream_kms_move_cursor(gdpy);
        return;
    }

    fds = qemu_dmabuf_get_fds(dmabuf, &n_fds);
    if (n_fds < 1 || fds[0] < 0) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: cursor missing dmabuf fd");
        return;
    }

    width = qemu_dmabuf_get_width(dmabuf);
    height = qemu_dmabuf_get_height(dmabuf);
    if (drmPrimeFDToHandle(gdpy->kms_fd, fds[0], &handle) != 0) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: cursor drmPrimeFDToHandle failed: %s",
                    strerror(errno));
        return;
    }

    if (drmModeSetCursor2(gdpy->kms_fd, gdpy->kms_crtc_id, handle,
                          width, height,
                          gdpy->kms_cursor_hot_x,
                          gdpy->kms_cursor_hot_y) != 0) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: SetCursor2 %ux%u failed: %s",
                    width, height, strerror(errno));
        drmCloseBufferHandle(gdpy->kms_fd, handle);
        return;
    }

    if (gdpy->kms_cursor_handle) {
        drmCloseBufferHandle(gdpy->kms_fd, gdpy->kms_cursor_handle);
    }
    gdpy->kms_cursor_handle = handle;
    gdpy->kms_cursor_width = width;
    gdpy->kms_cursor_height = height;
    gvt_stream_kms_move_cursor(gdpy);

    if (gdpy->verbose || gdpy->cursor_count <= 8) {
        error_report("gvt-stream-kms: cursor-ok size=%ux%u hot=%u,%u pos=%u,%u",
                     width, height, gdpy->kms_cursor_hot_x,
                     gdpy->kms_cursor_hot_y, gdpy->kms_cursor_pos_x,
                     gdpy->kms_cursor_pos_y);
    }
}

static void gvt_stream_kms_restore(GVTStreamDisplay *gdpy)
{
    if (gdpy->kms_fd < 0) {
        return;
    }
    if (gdpy->kms_old_crtc) {
        drmModeSetCrtc(gdpy->kms_fd, gdpy->kms_old_crtc->crtc_id,
                       gdpy->kms_old_crtc->buffer_id,
                       gdpy->kms_old_crtc->x, gdpy->kms_old_crtc->y,
                       gdpy->kms_old_crtc->buffer_id ?
                       &gdpy->kms_connector_id : NULL,
                       gdpy->kms_old_crtc->buffer_id ? 1 : 0,
                       gdpy->kms_old_crtc->buffer_id ?
                       &gdpy->kms_old_crtc->mode : NULL);
    }
}

static void gvt_stream_kms_close(GVTStreamDisplay *gdpy)
{
    gvt_stream_kms_clear_cursor(gdpy);
    gvt_stream_kms_restore(gdpy);
    gvt_stream_kms_remove_fb(gdpy);
    if (gdpy->kms_fd >= 0 && gdpy->kms_mode_blob_id) {
        drmModeDestroyPropertyBlob(gdpy->kms_fd, gdpy->kms_mode_blob_id);
    }
    g_clear_pointer(&gdpy->kms_old_crtc, drmModeFreeCrtc);
    if (gdpy->kms_fd >= 0) {
        close(gdpy->kms_fd);
    }
    gdpy->kms_fd = -1;
    gdpy->kms_initialized = false;
    gdpy->kms_connector_id = 0;
    gdpy->kms_crtc_id = 0;
    gdpy->kms_crtc_index = UINT32_MAX;
    gdpy->kms_primary_plane_id = 0;
    gdpy->kms_mode_blob_id = 0;
    gdpy->kms_conn_crtc_id_prop = 0;
    gdpy->kms_crtc_active_prop = 0;
    gdpy->kms_crtc_mode_id_prop = 0;
    gdpy->kms_plane_fb_id_prop = 0;
    gdpy->kms_plane_crtc_id_prop = 0;
    gdpy->kms_plane_src_x_prop = 0;
    gdpy->kms_plane_src_y_prop = 0;
    gdpy->kms_plane_src_w_prop = 0;
    gdpy->kms_plane_src_h_prop = 0;
    gdpy->kms_plane_crtc_x_prop = 0;
    gdpy->kms_plane_crtc_y_prop = 0;
    gdpy->kms_plane_crtc_w_prop = 0;
    gdpy->kms_plane_crtc_h_prop = 0;
    gdpy->kms_width = 0;
    gdpy->kms_height = 0;
    gdpy->kms_atomic_ready = false;
    gdpy->kms_atomic_active = false;
}

static bool gvt_stream_kms_init(GVTStreamDisplay *gdpy, QemuDmaBuf *dmabuf)
{
    drmModeRes *res = NULL;
    drmModeConnector *conn = NULL;
    char name[64];
    uint32_t width = qemu_dmabuf_get_width(dmabuf);
    uint32_t height = qemu_dmabuf_get_height(dmabuf);
    bool ok = false;

    if (!gdpy->kms_connector || !*gdpy->kms_connector ||
        gdpy->kms_disabled) {
        return false;
    }
    if (gdpy->kms_initialized &&
        gdpy->kms_width == width && gdpy->kms_height == height) {
        return true;
    }

    gvt_stream_kms_close(gdpy);
    gdpy->kms_fd = open(gdpy->kms_device ?: "/dev/dri/card0",
                        O_RDWR | O_CLOEXEC);
    if (gdpy->kms_fd < 0) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: open %s failed: %s",
                    gdpy->kms_device ?: "/dev/dri/card0", strerror(errno));
        gdpy->kms_disabled = true;
        return false;
    }

    res = drmModeGetResources(gdpy->kms_fd);
    if (!res) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: drmModeGetResources failed: %s",
                    strerror(errno));
        goto out;
    }

    conn = gvt_stream_kms_find_connector(gdpy->kms_fd, res,
                                         gdpy->kms_connector);
    if (!conn) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: connector %s not connected",
                    gdpy->kms_connector);
        goto out;
    }
    gvt_stream_kms_connector_name(conn, name, sizeof(name));

    if (!gvt_stream_kms_pick_mode(conn, width, height, &gdpy->kms_mode)) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: connector %s has no exact %ux%u mode",
                    name, width, height);
        goto out;
    }

    gdpy->kms_crtc_id = gvt_stream_kms_pick_crtc(gdpy->kms_fd, res, conn);
    if (!gdpy->kms_crtc_id) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: no CRTC for connector %s", name);
        goto out;
    }
    gdpy->kms_crtc_index = gvt_stream_kms_crtc_index(res, gdpy->kms_crtc_id);
    if (gdpy->kms_crtc_index == UINT32_MAX) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: no CRTC index for connector %s", name);
        goto out;
    }

    gdpy->kms_connector_id = conn->connector_id;
    gdpy->kms_width = width;
    gdpy->kms_height = height;
    gdpy->kms_old_crtc = drmModeGetCrtc(gdpy->kms_fd, gdpy->kms_crtc_id);
    gdpy->kms_atomic_ready = gvt_stream_kms_setup_atomic(gdpy, dmabuf);
    gdpy->kms_initialized = true;
    ok = true;

    error_report("gvt-stream-kms: enabled connector=%s id=%u crtc=%u "
                 "mode=%ux%u atomic=%d",
                 name, gdpy->kms_connector_id, gdpy->kms_crtc_id,
                 gdpy->kms_mode.hdisplay, gdpy->kms_mode.vdisplay,
                 gdpy->kms_atomic_ready);
    gvt_stream_kms_update_cursor(gdpy);

out:
    if (conn) {
        drmModeFreeConnector(conn);
    }
    if (res) {
        drmModeFreeResources(res);
    }
    if (!ok) {
        gvt_stream_kms_close(gdpy);
    }
    return ok;
}

static void gvt_stream_kms_present(GVTStreamDisplay *gdpy)
{
    QemuDmaBuf *dmabuf = gdpy->scanout;
    const int *fds;
    const uint32_t *strides;
    const uint32_t *offsets;
    int n_fds = 0, n_strides = 0, n_offsets = 0;
    uint32_t handles[4] = { 0 };
    uint32_t fb_strides[4] = { 0 };
    uint32_t fb_offsets[4] = { 0 };
    uint64_t modifiers[4] = { 0 };
    uint32_t planes;
    uint32_t fb_id = 0;
    uint32_t old_fb_id = 0;
    uint32_t i;
    bool used_atomic = false;
    bool page_flip_queued = false;
    bool used_page_flip = false;
    bool used_modeset = false;

    if (!gdpy->kms_connector || !*gdpy->kms_connector || !dmabuf) {
        return;
    }
    if (!gvt_stream_kms_init(gdpy, dmabuf)) {
        return;
    }
    if (gdpy->kms_fb_dmabuf == dmabuf && gdpy->kms_fb_id) {
        return;
    }

    planes = qemu_dmabuf_get_num_planes(dmabuf);
    if (planes > 4) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: unsupported plane count %u", planes);
        return;
    }

    fds = qemu_dmabuf_get_fds(dmabuf, &n_fds);
    strides = qemu_dmabuf_get_strides(dmabuf, &n_strides);
    offsets = qemu_dmabuf_get_offsets(dmabuf, &n_offsets);
    for (i = 0; i < planes; i++) {
        if (i >= n_fds || fds[i] < 0 ||
            i >= n_strides || i >= n_offsets) {
            gdpy->kms_fail_count++;
            warn_report("gvt-stream-kms: incomplete dmabuf plane metadata");
            return;
        }
        if (drmPrimeFDToHandle(gdpy->kms_fd, fds[i], &handles[i]) != 0) {
            gdpy->kms_fail_count++;
            warn_report("gvt-stream-kms: drmPrimeFDToHandle failed: %s",
                        strerror(errno));
            goto out_handles;
        }
        fb_strides[i] = strides[i];
        fb_offsets[i] = offsets[i];
        modifiers[i] = qemu_dmabuf_get_modifier(dmabuf);
    }

    if (drmModeAddFB2WithModifiers(gdpy->kms_fd,
                                   qemu_dmabuf_get_width(dmabuf),
                                   qemu_dmabuf_get_height(dmabuf),
                                   qemu_dmabuf_get_fourcc(dmabuf),
                                   handles, fb_strides, fb_offsets, modifiers,
                                   &fb_id, DRM_MODE_FB_MODIFIERS) != 0) {
        gdpy->kms_fail_count++;
        warn_report("gvt-stream-kms: AddFB2WithModifiers failed: %s",
                    strerror(errno));
        goto out_handles;
    }

    old_fb_id = gdpy->kms_fb_id;
    if (gdpy->kms_atomic_ready &&
        gvt_stream_kms_atomic_commit(gdpy, fb_id)) {
        used_atomic = true;
    }

    if (!used_atomic && old_fb_id) {
        GVTStreamKmsFlipWait wait = { 0 };

        if (drmModePageFlip(gdpy->kms_fd, gdpy->kms_crtc_id, fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, &wait) == 0) {
            page_flip_queued = true;
            used_page_flip = gvt_stream_kms_wait_page_flip(gdpy, &wait);
            if (!used_page_flip) {
                warn_report("gvt-stream-kms: PageFlip queued but completion "
                            "was not confirmed");
            }
        } else {
            if (gdpy->verbose || gdpy->kms_commit_count <= 8) {
                warn_report("gvt-stream-kms: PageFlip failed, fallback "
                            "to SetCrtc: %s", strerror(errno));
            }
        }
    }

    if (!used_atomic && (!old_fb_id || !page_flip_queued)) {
        if (drmModeSetCrtc(gdpy->kms_fd, gdpy->kms_crtc_id, fb_id, 0, 0,
                           &gdpy->kms_connector_id, 1,
                           &gdpy->kms_mode) != 0) {
            gdpy->kms_fail_count++;
            warn_report("gvt-stream-kms: SetCrtc failed: %s", strerror(errno));
            drmModeRmFB(gdpy->kms_fd, fb_id);
            goto out_handles;
        }
        used_modeset = true;
    }

    if (old_fb_id) {
        drmModeRmFB(gdpy->kms_fd, old_fb_id);
    }
    gdpy->kms_fb_id = fb_id;
    gdpy->kms_fb_dmabuf = dmabuf;
    gdpy->kms_commit_count++;
    if (used_atomic) {
        gdpy->kms_atomic_count++;
    }
    if (page_flip_queued) {
        gdpy->kms_page_flip_count++;
    }
    if (used_modeset) {
        gdpy->kms_modeset_count++;
    }
    gvt_stream_kms_update_cursor(gdpy);
    if (gdpy->verbose || gdpy->kms_commit_count <= 8 ||
        gdpy->kms_commit_count % 60 == 0) {
        const char *method = used_atomic ? "atomic" :
                             page_flip_queued ? "page-flip" : "modeset";

        error_report("gvt-stream-kms: present-ok #%" PRIu64
                     " fb=%u size=%ux%u method=%s",
                     gdpy->kms_commit_count, gdpy->kms_fb_id,
                     qemu_dmabuf_get_width(dmabuf),
                     qemu_dmabuf_get_height(dmabuf),
                     method);
    }

out_handles:
    for (i = 0; i < planes; i++) {
        if (handles[i]) {
            drmCloseBufferHandle(gdpy->kms_fd, handles[i]);
        }
    }
}


static uint64_t gvt_stream_checksum_surface(DisplaySurface *surface)
{
    uint8_t *data = surface_data(surface);
    int width = surface_width(surface);
    int height = surface_height(surface);
    int stride = surface_stride(surface);
    uint64_t hash = 1469598103934665603ULL;
    int x, y, b;

    for (y = 0; y < height; y++) {
        const uint8_t *row = data + y * stride;

        for (x = 0; x < width; x++) {
            for (b = 0; b < 4; b++) {
                hash ^= row[x * 4 + b];
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

static void gvt_stream_update_idle_sample(GVTStreamDisplay *gdpy,
                                          DisplaySurface *surface,
                                          int64_t now_ms)
{
    uint8_t *data = surface_data(surface);
    int width = surface_width(surface);
    int height = surface_height(surface);
    int stride = surface_stride(surface);
    uint32_t sample[GVT_STREAM_IDLE_SAMPLE_N];
    uint64_t changed = 0;
    int x, y, sx, sy, idx;

    if (!data || width <= 0 || height <= 0) {
        return;
    }

    for (sy = 0; sy < GVT_STREAM_IDLE_SAMPLE_H; sy++) {
        y = GVT_STREAM_IDLE_SAMPLE_H == 1 ? 0 :
            (int)((int64_t)sy * (height - 1) / (GVT_STREAM_IDLE_SAMPLE_H - 1));
        for (sx = 0; sx < GVT_STREAM_IDLE_SAMPLE_W; sx++) {
            const uint8_t *p;

            x = GVT_STREAM_IDLE_SAMPLE_W == 1 ? 0 :
                (int)((int64_t)sx * (width - 1) / (GVT_STREAM_IDLE_SAMPLE_W - 1));
            p = data + (size_t)y * stride + (size_t)x * 4;
            idx = sy * GVT_STREAM_IDLE_SAMPLE_W + sx;
            sample[idx] = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];

            if (gdpy->idle_sample_valid) {
                uint32_t old = gdpy->idle_sample[idx];
                int db = abs((int)(old & 0xff) - (int)(sample[idx] & 0xff));
                int dg = abs((int)((old >> 8) & 0xff) -
                             (int)((sample[idx] >> 8) & 0xff));
                int dr = abs((int)((old >> 16) & 0xff) -
                             (int)((sample[idx] >> 16) & 0xff));

                if ((uint64_t)(db + dg + dr) > gdpy->idle_pixel_delta * 3) {
                    changed++;
                }
            }
        }
    }

    if (!gdpy->idle_sample_valid) {
        memcpy(gdpy->idle_sample, sample, sizeof(sample));
        gdpy->idle_sample_valid = true;
        gdpy->last_content_change_ms = now_ms;
        gdpy->last_probe_diff_ppm = 1000000;
        return;
    }

    gdpy->last_probe_diff_ppm =
        changed * 1000000ULL / GVT_STREAM_IDLE_SAMPLE_N;
    if (gdpy->last_probe_diff_ppm > gdpy->idle_changed_ppm) {
        if (gvt_stream_effective_capture_ms(gdpy, now_ms) > gdpy->capture_ms) {
            gdpy->idle_wake_count++;
        }
        gdpy->last_content_change_ms = now_ms;
        memcpy(gdpy->idle_sample, sample, sizeof(sample));
    }
}

static void gvt_stream_probe_activity(GVTStreamDisplay *gdpy,
                                      QemuDmaBuf *dmabuf,
                                      int64_t now_ms)
{
#ifdef CONFIG_GBM
    uint32_t width, height, texture;

    if (!gdpy->idle_probe_ms || !dmabuf ||
        (gdpy->last_probe_ms &&
         now_ms - gdpy->last_probe_ms < gdpy->idle_probe_ms)) {
        return;
    }

    gdpy->last_probe_ms = now_ms;
    gdpy->idle_probe_count++;

    egl_dmabuf_import_texture(dmabuf);
    texture = qemu_dmabuf_get_texture(dmabuf);
    if (!texture) {
        gdpy->capture_fail_count++;
        return;
    }

    width = qemu_dmabuf_get_width(dmabuf);
    height = qemu_dmabuf_get_height(dmabuf);
    if (!width || !height) {
        return;
    }

    if (gdpy->guest_fb.texture != texture ||
        gdpy->guest_fb.width != width || gdpy->guest_fb.height != height) {
        egl_fb_destroy(&gdpy->guest_fb);
        egl_fb_setup_for_tex(&gdpy->guest_fb, width, height, texture, false);
        gdpy->guest_fb.dmabuf = dmabuf;
    }

    if (gdpy->capture_fb.width != width || gdpy->capture_fb.height != height) {
        egl_fb_destroy(&gdpy->capture_fb);
        egl_fb_setup_new_tex(&gdpy->capture_fb, width, height);
    }

    if (!gdpy->capture_surface ||
        surface_width(gdpy->capture_surface) != width ||
        surface_height(gdpy->capture_surface) != height) {
        g_clear_pointer(&gdpy->capture_surface, qemu_free_displaysurface);
        gdpy->capture_surface = qemu_create_displaysurface(width, height);
    }

    egl_fb_blit(&gdpy->capture_fb, &gdpy->guest_fb,
                qemu_dmabuf_get_y0_top(dmabuf));
    egl_fb_read(gdpy->capture_surface, &gdpy->capture_fb);
    gdpy->last_capture_checksum =
        gvt_stream_checksum_surface(gdpy->capture_surface);
    gvt_stream_update_idle_sample(gdpy, gdpy->capture_surface, now_ms);
#endif
}

static bool gvt_stream_write_ppm(GVTStreamDisplay *gdpy, const char *path)
{
    DisplaySurface *surface = gdpy->capture_surface;
    FILE *fp;
    uint8_t *line;
    uint8_t *data;
    int width;
    int height;
    int stride;
    int x, y;
    bool ok = true;

    fp = fopen(path, "wb");
    if (!fp) {
        error_report("gvt-stream: capture-open-failed path=%s error=%s",
                     path, strerror(errno));
        return false;
    }

    width = surface_width(surface);
    height = surface_height(surface);
    stride = surface_stride(surface);
    data = surface_data(surface);
    line = g_malloc(width * 3);

    if (fprintf(fp, "P6\n%d %d\n255\n", width, height) < 0) {
        ok = false;
        goto out;
    }

    for (y = 0; y < height; y++) {
        const uint8_t *src = data + (height - 1 - y) * stride;

        for (x = 0; x < width; x++) {
            line[x * 3 + 0] = src[x * 4 + 2];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + 0];
        }
        if (fwrite(line, width * 3, 1, fp) != 1) {
            ok = false;
            break;
        }
    }

out:
    if (fclose(fp) != 0) {
        ok = false;
    }
    g_free(line);
    if (!ok) {
        error_report("gvt-stream: capture-write-failed path=%s error=%s",
                     path, strerror(errno));
    }
    return ok;
}


static bool gvt_stream_encoder_start(GVTStreamDisplay *gdpy,
                                     int width, int height)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *pipeline_desc = NULL;
    GstCaps *caps;
    GstStateChangeReturn state_ret;

    if (!gdpy->encode_file && !(gdpy->rtp_host && gdpy->rtp_port)) {
        return false;
    }
    if (gdpy->encode_pipeline) {
        return true;
    }

    gst_init(NULL, NULL);

    if (gdpy->rtp_host && gdpy->rtp_port &&
        (gdpy->rtp_fec || gdpy->rtp_fec_important)) {
        pipeline_desc = g_strdup_printf(
            "appsrc name=src is-live=true format=time do-timestamp=false block=false "
            "! queue leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 "
            "! vaapipostproc format=nv12 scale-method=fast "
            "! video/x-raw(memory:VASurface),format=NV12 "
            "! vaapih264enc rate-control=cbr bitrate=%d keyframe-period=%d "
            "max-bframes=0 refs=1 cabac=false aud=true "
            "! h264parse config-interval=1 "
            "! rtph264pay pt=96 ssrc=2222 config-interval=1 mtu=1000 "
            "! rtpulpfecenc pt=122 percentage=%u percentage-important=%u multipacket=true "
            "! udpsink host=%s port=%u sync=false async=false",
            gdpy->encode_bitrate, gdpy->encode_keyint,
            (unsigned)gdpy->rtp_fec, (unsigned)gdpy->rtp_fec_important,
            gdpy->rtp_host, (unsigned)gdpy->rtp_port);
    } else if (gdpy->rtp_host && gdpy->rtp_port) {
        pipeline_desc = g_strdup_printf(
            "appsrc name=src is-live=true format=time do-timestamp=false block=false "
            "! queue leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 "
            "! vaapipostproc format=nv12 scale-method=fast "
            "! video/x-raw(memory:VASurface),format=NV12 "
            "! vaapih264enc rate-control=cbr bitrate=%d keyframe-period=%d "
            "max-bframes=0 refs=1 cabac=false aud=true "
            "! h264parse config-interval=1 "
            "! rtph264pay pt=96 ssrc=2222 config-interval=1 mtu=1000 "
            "! udpsink host=%s port=%u sync=false async=false",
            gdpy->encode_bitrate, gdpy->encode_keyint,
            gdpy->rtp_host, (unsigned)gdpy->rtp_port);
    } else {
        pipeline_desc = g_strdup_printf(
            "appsrc name=src is-live=true format=time do-timestamp=false block=false "
            "! queue leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 "
            "! vaapipostproc format=nv12 scale-method=fast "
            "! video/x-raw(memory:VASurface),format=NV12 "
            "! vaapih264enc rate-control=cbr bitrate=%d keyframe-period=%d "
            "max-bframes=0 refs=1 cabac=false aud=true "
            "! h264parse config-interval=1 "
            "! video/x-h264,stream-format=byte-stream,alignment=au "
            "! filesink location=%s sync=false async=false",
            gdpy->encode_bitrate, gdpy->encode_keyint, gdpy->encode_file);
    }

    gdpy->encode_pipeline = gst_parse_launch(pipeline_desc, &error);
    if (error) {
        gdpy->encode_fail_count++;
        error_report("gvt-stream: encode-pipeline-create-failed desc=%s error=%s",
                     pipeline_desc, error->message);
        if (gdpy->encode_pipeline) {
            gst_object_unref(gdpy->encode_pipeline);
            gdpy->encode_pipeline = NULL;
        }
        return false;
    }
    if (!gdpy->encode_pipeline) {
        gdpy->encode_fail_count++;
        error_report("gvt-stream: encode-pipeline-create-failed desc=%s error=unknown",
                     pipeline_desc);
        return false;
    }

    gdpy->encode_appsrc = gst_bin_get_by_name(GST_BIN(gdpy->encode_pipeline),
                                              "src");
    if (!gdpy->encode_appsrc) {
        gdpy->encode_fail_count++;
        error_report("gvt-stream: encode-appsrc-not-found");
        gst_object_unref(gdpy->encode_pipeline);
        gdpy->encode_pipeline = NULL;
        return false;
    }

    caps = gst_caps_new_simple("video/x-raw",
                               "format", G_TYPE_STRING, "BGRx",
                               "width", G_TYPE_INT, width,
                               "height", G_TYPE_INT, height,
                               "framerate", GST_TYPE_FRACTION,
                               gdpy->encode_fps, 1,
                               NULL);
    if (gdpy->encode_dmabuf && gdpy->encode_dmabuf_caps_feature) {
        gst_caps_set_features(caps, 0,
                              gst_caps_features_new("memory:DMABuf", NULL));
    }
    gst_app_src_set_caps(GST_APP_SRC(gdpy->encode_appsrc), caps);
    gst_caps_unref(caps);

    gdpy->encode_duration = gst_util_uint64_scale_int(1, GST_SECOND,
                                                      gdpy->encode_fps);
    state_ret = gst_element_set_state(gdpy->encode_pipeline,
                                      GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        gdpy->encode_fail_count++;
        error_report("gvt-stream: encode-pipeline-start-failed");
        gst_object_unref(gdpy->encode_appsrc);
        gdpy->encode_appsrc = NULL;
        gst_object_unref(gdpy->encode_pipeline);
        gdpy->encode_pipeline = NULL;
        return false;
    }

    if (gdpy->rtp_host && gdpy->rtp_port) {
        error_report("gvt-stream: encode-start rtp=%s:%u size=%dx%d fps=%d "
                     "bitrate=%d keyint=%d fec=%u/%u path=%s dmabuf_caps=%d flip=%d",
                     gdpy->rtp_host, (unsigned)gdpy->rtp_port, width, height,
                     gdpy->encode_fps, gdpy->encode_bitrate, gdpy->encode_keyint,
                     (unsigned)gdpy->rtp_fec, (unsigned)gdpy->rtp_fec_important,
                     gdpy->encode_dmabuf ? "dmabuf" : "cpu",
                     gdpy->encode_dmabuf_caps_feature, gdpy->encode_flip);
    } else {
        error_report("gvt-stream: encode-start file=%s size=%dx%d fps=%d "
                     "bitrate=%d keyint=%d path=%s dmabuf_caps=%d flip=%d",
                     gdpy->encode_file, width, height, gdpy->encode_fps,
                     gdpy->encode_bitrate, gdpy->encode_keyint,
                     gdpy->encode_dmabuf ? "dmabuf" : "cpu",
                     gdpy->encode_dmabuf_caps_feature, gdpy->encode_flip);
    }
    return true;
}

static void gvt_stream_encoder_finish(GVTStreamDisplay *gdpy)
{
    GstBus *bus;
    GstMessage *msg;

    if (!gdpy->encode_pipeline || !gdpy->encode_appsrc) {
        return;
    }

    gst_app_src_end_of_stream(GST_APP_SRC(gdpy->encode_appsrc));
    bus = gst_element_get_bus(gdpy->encode_pipeline);
    msg = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
                                     GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = NULL;
            gchar *debug = NULL;

            gdpy->encode_fail_count++;
            gst_message_parse_error(msg, &err, &debug);
            error_report("gvt-stream: encode-error message=%s debug=%s",
                         err ? err->message : "unknown", debug ? debug : "");
            g_clear_error(&err);
            g_free(debug);
        } else {
            error_report("gvt-stream: encode-eos-received frames=%" PRIu64
                         " target=%s",
                         gdpy->encode_count, gdpy->encode_file ?: gdpy->rtp_host);
        }
        gst_message_unref(msg);
    } else {
        gdpy->encode_fail_count++;
        warn_report("gvt-stream: encode-eos-timeout frames=%" PRIu64
                    " target=%s", gdpy->encode_count,
                    gdpy->encode_file ?: gdpy->rtp_host);
    }
    gst_object_unref(bus);
    gst_element_set_state(gdpy->encode_pipeline, GST_STATE_NULL);
    gst_object_unref(gdpy->encode_appsrc);
    gst_object_unref(gdpy->encode_pipeline);
    gdpy->encode_appsrc = NULL;
    gdpy->encode_pipeline = NULL;
    error_report("gvt-stream: encode-finish frames=%" PRIu64 " target=%s",
                 gdpy->encode_count, gdpy->encode_file ?: gdpy->rtp_host);
}

static void gvt_stream_stamp_buffer(GVTStreamDisplay *gdpy, GstBuffer *buf,
                                    int64_t now_ms)
{
    GstClockTime duration = gdpy->encode_duration;

    if (gdpy->last_encode_wall_ms) {
        int64_t delta_ms = now_ms - gdpy->last_encode_wall_ms;

        if (delta_ms < 1) {
            delta_ms = 1;
        } else if (delta_ms > 1000) {
            delta_ms = 1000;
        }
        duration = (GstClockTime)delta_ms * GST_MSECOND;
    }

    GST_BUFFER_PTS(buf) = gdpy->encode_pts;
    GST_BUFFER_DTS(buf) = gdpy->encode_pts;
    GST_BUFFER_DURATION(buf) = duration;
    gdpy->encode_pts += duration;
    gdpy->last_encode_wall_ms = now_ms;
}

static void gvt_stream_encoder_push_dmabuf(GVTStreamDisplay *gdpy,
                                           QemuDmaBuf *dmabuf,
                                           int64_t now_ms)
{
    GstBuffer *buf;
    GstMemory *mem;
    GstFlowReturn flow;
    const int *fds;
    const uint32_t *offsets;
    const uint32_t *strides;
    int n_fds = 0, n_offsets = 0, n_strides = 0;
    int fd;
    uint32_t width, height, fourcc, stride, offset;
    gsize plane_offsets[1];
    gint plane_strides[1];
    size_t size;

    if ((!gdpy->encode_file && !(gdpy->rtp_host && gdpy->rtp_port)) || !dmabuf) {
        return;
    }
    if (gdpy->encode_max && gdpy->encode_count >= gdpy->encode_max) {
        return;
    }

    fds = qemu_dmabuf_get_fds(dmabuf, &n_fds);
    offsets = qemu_dmabuf_get_offsets(dmabuf, &n_offsets);
    strides = qemu_dmabuf_get_strides(dmabuf, &n_strides);
    width = qemu_dmabuf_get_width(dmabuf);
    height = qemu_dmabuf_get_height(dmabuf);
    fourcc = qemu_dmabuf_get_fourcc(dmabuf);
    if (!fds || n_fds < 1 || !strides || n_strides < 1 ||
        !width || !height || fourcc != 0x34325258) {
        gdpy->encode_fail_count++;
        error_report("gvt-stream: dmabuf-push-unsupported fds=%d strides=%d "
                     "size=%ux%u fourcc=0x%08x",
                     n_fds, n_strides, width, height, fourcc);
        return;
    }

    offset = (offsets && n_offsets > 0) ? offsets[0] : 0;
    stride = strides[0];
    size = (size_t)offset + (size_t)stride * height;

    if (!gvt_stream_encoder_start(gdpy, width, height)) {
        return;
    }
    if (!gdpy->dmabuf_allocator) {
        gdpy->dmabuf_allocator = gst_dmabuf_allocator_new();
        if (!gdpy->dmabuf_allocator) {
            gdpy->encode_fail_count++;
            error_report("gvt-stream: dmabuf-allocator-create-failed");
            return;
        }
    }

    fd = dup(fds[0]);
    if (fd < 0) {
        gdpy->encode_fail_count++;
        error_report("gvt-stream: dmabuf-dup-failed fd=%d error=%s",
                     fds[0], strerror(errno));
        return;
    }

    mem = gst_dmabuf_allocator_alloc(gdpy->dmabuf_allocator, fd, size);
    if (!mem) {
        gdpy->encode_fail_count++;
        close(fd);
        error_report("gvt-stream: dmabuf-memory-alloc-failed fd=%d size=%zu",
                     fds[0], size);
        return;
    }

    buf = gst_buffer_new();
    gst_buffer_append_memory(buf, mem);
    plane_offsets[0] = offset;
    plane_strides[0] = stride;
    gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE,
                                   GST_VIDEO_FORMAT_BGRx,
                                   width, height, 1,
                                   plane_offsets, plane_strides);

    gvt_stream_stamp_buffer(gdpy, buf, now_ms);

    flow = gst_app_src_push_buffer(GST_APP_SRC(gdpy->encode_appsrc), buf);
    if (flow != GST_FLOW_OK) {
        gdpy->encode_fail_count++;
        error_report("gvt-stream: dmabuf-push-failed flow=%s",
                     gst_flow_get_name(flow));
        return;
    }

    gdpy->encode_count++;
    gdpy->encode_dmabuf_count++;
    if (gdpy->verbose || gdpy->encode_max || gdpy->encode_count <= 5 ||
        gdpy->encode_count % 60 == 0) {
        error_report("gvt-stream: dmabuf-push-ok #=%" PRIu64
                     " target=%s fd=%d stride=%u size=%zu",
                     gdpy->encode_count,
                     gdpy->encode_file ?: gdpy->rtp_host,
                     fds[0], stride, size);
    }
    if (gdpy->encode_max && gdpy->encode_count == gdpy->encode_max) {
        gvt_stream_encoder_finish(gdpy);
    }
}

static void gvt_stream_encoder_push(GVTStreamDisplay *gdpy, int64_t now_ms)
{
    DisplaySurface *surface = gdpy->capture_surface;
    GstBuffer *buf;
    GstMapInfo map;
    GstFlowReturn flow;
    uint8_t *src_data;
    int width, height, stride, y;
    size_t frame_size;

    if ((!gdpy->encode_file && !(gdpy->rtp_host && gdpy->rtp_port)) || !surface) {
        return;
    }
    if (gdpy->encode_max && gdpy->encode_count >= gdpy->encode_max) {
        return;
    }

    width = surface_width(surface);
    height = surface_height(surface);
    stride = surface_stride(surface);
    frame_size = (size_t)width * height * 4;

    if (!gvt_stream_encoder_start(gdpy, width, height)) {
        return;
    }

    buf = gst_buffer_new_allocate(NULL, frame_size, NULL);
    if (!buf) {
        gdpy->encode_fail_count++;
        error_report("gvt-stream: encode-buffer-alloc-failed");
        return;
    }
    if (!gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        gdpy->encode_fail_count++;
        error_report("gvt-stream: encode-buffer-map-failed");
        gst_buffer_unref(buf);
        return;
    }

    src_data = surface_data(surface);
    for (y = 0; y < height; y++) {
        int src_y = gdpy->encode_flip ? height - 1 - y : y;
        memcpy(map.data + (size_t)y * width * 4,
               src_data + (size_t)src_y * stride,
               (size_t)width * 4);
    }
    gst_buffer_unmap(buf, &map);

    gvt_stream_stamp_buffer(gdpy, buf, now_ms);

    flow = gst_app_src_push_buffer(GST_APP_SRC(gdpy->encode_appsrc), buf);
    if (flow != GST_FLOW_OK) {
        gdpy->encode_fail_count++;
        error_report("gvt-stream: encode-push-failed flow=%s",
                     gst_flow_get_name(flow));
        return;
    }

    gdpy->encode_count++;
    gdpy->encode_cpu_count++;
    if (gdpy->verbose || gdpy->encode_max || gdpy->encode_count <= 5 ||
        gdpy->encode_count % 60 == 0) {
        error_report("gvt-stream: encode-push-ok #=%" PRIu64
                     " target=%s checksum=0x%016" PRIx64,
                     gdpy->encode_count,
                     gdpy->encode_file ?: gdpy->rtp_host,
                     gdpy->last_capture_checksum);
    }

    if (gdpy->encode_max && gdpy->encode_count == gdpy->encode_max) {
        gvt_stream_encoder_finish(gdpy);
    }
}

static void gvt_stream_capture_frame(GVTStreamDisplay *gdpy, int64_t now_ms)
{
#ifdef CONFIG_GBM
    QemuDmaBuf *dmabuf = gdpy->scanout;
    uint32_t width, height, texture;
    g_autofree char *path = NULL;
    bool had_texture;
    uint64_t capture_ms;

    if ((!gdpy->capture_dir && !gdpy->encode_file &&
         !(gdpy->rtp_host && gdpy->rtp_port)) || !dmabuf) {
        return;
    }
    if (gdpy->capture_max && gdpy->capture_count >= gdpy->capture_max) {
        return;
    }

    gvt_stream_probe_activity(gdpy, dmabuf, now_ms);
    capture_ms = gvt_stream_effective_capture_ms(gdpy, now_ms);
    if (gdpy->last_capture_ms &&
        now_ms - gdpy->last_capture_ms < capture_ms) {
        return;
    }

    if (gdpy->encode_dmabuf && !gdpy->capture_dir) {
        gdpy->capture_count++;
        gdpy->last_capture_ms = now_ms;
        gvt_stream_encoder_push_dmabuf(gdpy, dmabuf, now_ms);
        return;
    }

    had_texture = qemu_dmabuf_get_texture(dmabuf) != 0;
    egl_dmabuf_import_texture(dmabuf);
    texture = qemu_dmabuf_get_texture(dmabuf);
    if (!texture) {
        gdpy->capture_fail_count++;
        error_report("gvt-stream: capture-import-failed failures=%" PRIu64,
                     gdpy->capture_fail_count);
        return;
    }
    if (!had_texture) {
        gdpy->import_count++;
        error_report("gvt-stream: capture-import-ok #%" PRIu64
                     " console=%d dmabuf=%p texture=%u",
                     gdpy->import_count,
                     qemu_console_get_index(gdpy->dcl.con), dmabuf, texture);
    }

    width = qemu_dmabuf_get_width(dmabuf);
    height = qemu_dmabuf_get_height(dmabuf);

    if (gdpy->guest_fb.texture != texture ||
        gdpy->guest_fb.width != width || gdpy->guest_fb.height != height) {
        egl_fb_destroy(&gdpy->guest_fb);
        egl_fb_setup_for_tex(&gdpy->guest_fb, width, height, texture, false);
        gdpy->guest_fb.dmabuf = dmabuf;
    }

    if (gdpy->capture_fb.width != width || gdpy->capture_fb.height != height) {
        egl_fb_destroy(&gdpy->capture_fb);
        egl_fb_setup_new_tex(&gdpy->capture_fb, width, height);
    }

    if (!gdpy->capture_surface ||
        surface_width(gdpy->capture_surface) != width ||
        surface_height(gdpy->capture_surface) != height) {
        g_clear_pointer(&gdpy->capture_surface, qemu_free_displaysurface);
        gdpy->capture_surface = qemu_create_displaysurface(width, height);
    }

    egl_fb_blit(&gdpy->capture_fb, &gdpy->guest_fb,
                qemu_dmabuf_get_y0_top(dmabuf));
    egl_fb_read(gdpy->capture_surface, &gdpy->capture_fb);

    gdpy->last_capture_checksum =
        gvt_stream_checksum_surface(gdpy->capture_surface);
    gdpy->capture_count++;
    gdpy->last_capture_ms = now_ms;

    if (gdpy->capture_dir) {
        path = g_strdup_printf("%s/gvt-stream-%06" PRIu64 "-%ux%u.ppm",
                               gdpy->capture_dir, gdpy->capture_count,
                               width, height);
        if (!gvt_stream_write_ppm(gdpy, path)) {
            gdpy->capture_fail_count++;
            return;
        }

        error_report("gvt-stream: capture-ok #%" PRIu64 " console=%d path=%s "
                     "size=%ux%u checksum=0x%016" PRIx64,
                     gdpy->capture_count, qemu_console_get_index(gdpy->dcl.con),
                     path, width, height, gdpy->last_capture_checksum);
    }

    gvt_stream_encoder_push(gdpy, now_ms);
#else
    if (gdpy->capture_dir) {
        gdpy->capture_fail_count++;
        error_report("gvt-stream: capture unavailable without GBM");
    }
#endif
}

static void gvt_stream_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);
}

static void gvt_stream_gfx_update(DisplayChangeListener *dcl,
                                  int x, int y, int w, int h)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    if (gdpy->verbose) {
        error_report("gvt-stream: gfx-update console=%d rect=%d,%d %dx%d",
                     qemu_console_get_index(dcl->con), x, y, w, h);
    }
}

static void gvt_stream_gfx_switch(DisplayChangeListener *dcl,
                                  DisplaySurface *surface)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    if (!surface) {
        error_report("gvt-stream: gfx-switch console=%d surface=NULL",
                     qemu_console_get_index(dcl->con));
        return;
    }

    if (gdpy->verbose || surface_is_placeholder(surface)) {
        error_report("gvt-stream: gfx-switch console=%d surface=%dx%d "
                     "format=0x%x placeholder=%d",
                     qemu_console_get_index(dcl->con),
                     surface_width(surface), surface_height(surface),
                     surface_format(surface), surface_is_placeholder(surface));
    }
}

static void gvt_stream_scanout_disable(DisplayChangeListener *dcl)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    gdpy->scanout = NULL;
    gvt_stream_kms_close(gdpy);
    egl_fb_destroy(&gdpy->guest_fb);
    egl_fb_destroy(&gdpy->capture_fb);
    g_clear_pointer(&gdpy->capture_surface, qemu_free_displaysurface);
    error_report("gvt-stream: scanout-disable console=%d",
                 qemu_console_get_index(dcl->con));
}

static void gvt_stream_scanout_texture(DisplayChangeListener *dcl,
                                       uint32_t backing_id,
                                       bool backing_y_0_top,
                                       uint32_t backing_width,
                                       uint32_t backing_height,
                                       uint32_t x, uint32_t y,
                                       uint32_t w, uint32_t h,
                                       void *d3d_tex2d)
{
    error_report("gvt-stream: scanout-texture console=%d tex=%u "
                 "backing=%ux%u rect=%u,%u %ux%u y0_top=%d d3d=%p",
                 qemu_console_get_index(dcl->con), backing_id,
                 backing_width, backing_height, x, y, w, h,
                 backing_y_0_top, d3d_tex2d);
}

static bool gvt_stream_has_dmabuf(DisplayChangeListener *dcl)
{
    return true;
}

static void gvt_stream_scanout_dmabuf(DisplayChangeListener *dcl,
                                      QemuDmaBuf *dmabuf)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);
    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    gdpy->scanout = dmabuf;
    gdpy->scanout_count++;
    if (gdpy->verbose || gdpy->scanout_count <= 8) {
        gvt_stream_log_dmabuf(gdpy, "scanout-dmabuf", dmabuf);
    }

    if ((gdpy->import_test || gdpy->capture_dir) && dmabuf) {
#ifdef CONFIG_GBM
        egl_dmabuf_import_texture(dmabuf);
        if (qemu_dmabuf_get_texture(dmabuf)) {
            gdpy->import_count++;
            error_report("gvt-stream: import-test-ok #%" PRIu64
                         " console=%d dmabuf=%p texture=%u",
                         gdpy->import_count, qemu_console_get_index(dcl->con),
                         dmabuf, qemu_dmabuf_get_texture(dmabuf));
        } else {
            gdpy->import_fail_count++;
            error_report("gvt-stream: import-test-failed #%" PRIu64
                         " console=%d dmabuf=%p",
                         gdpy->import_fail_count,
                         qemu_console_get_index(dcl->con), dmabuf);
        }
#else
            gdpy->import_fail_count++;
            error_report("gvt-stream: import-test unavailable without GBM");
#endif
    }
    gvt_stream_publish_dmabuf(gdpy, dmabuf, now_ms, true);
}

static void gvt_stream_cursor_dmabuf(DisplayChangeListener *dcl,
                                     QemuDmaBuf *dmabuf, bool have_hot,
                                     uint32_t hot_x, uint32_t hot_y)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    gdpy->cursor_count++;
    gdpy->last_activity_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    gdpy->kms_cursor_dmabuf = dmabuf;
    gdpy->kms_cursor_hot_x = have_hot ? hot_x : 0;
    gdpy->kms_cursor_hot_y = have_hot ? hot_y : 0;
    if (!dmabuf) {
        gvt_stream_kms_clear_cursor(gdpy);
    } else {
        gvt_stream_kms_update_cursor(gdpy);
    }
    error_report("gvt-stream: cursor-dmabuf #%" PRIu64 " console=%d "
                 "dmabuf=%p have_hot=%d hot=%u,%u",
                 gdpy->cursor_count, qemu_console_get_index(dcl->con),
                 dmabuf, have_hot, hot_x, hot_y);
    if (gdpy->verbose && dmabuf) {
        gvt_stream_log_dmabuf(gdpy, "cursor-detail", dmabuf);
    }
}

static void gvt_stream_cursor_position(DisplayChangeListener *dcl,
                                       uint32_t pos_x, uint32_t pos_y)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    gdpy->last_activity_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    gdpy->kms_cursor_pos_x = pos_x;
    gdpy->kms_cursor_pos_y = pos_y;
    gvt_stream_kms_move_cursor(gdpy);
    if (gdpy->verbose) {
        error_report("gvt-stream: cursor-position console=%d pos=%u,%u",
                     qemu_console_get_index(dcl->con), pos_x, pos_y);
    }
}

static void gvt_stream_release_dmabuf(DisplayChangeListener *dcl,
                                      QemuDmaBuf *dmabuf)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);

    gdpy->release_count++;
    if (gdpy->kms_cursor_dmabuf == dmabuf) {
        gvt_stream_kms_clear_cursor(gdpy);
    }
    if (gdpy->kms_fb_dmabuf == dmabuf) {
        gdpy->kms_fb_dmabuf = NULL;
    }
    if (gdpy->last_publish_dmabuf == dmabuf) {
        gdpy->last_publish_dmabuf = NULL;
    }
    if ((gdpy->import_test || gdpy->capture_dir) && dmabuf) {
#ifdef CONFIG_GBM
        egl_dmabuf_release_texture(dmabuf);
#endif
    }
    if (gdpy->verbose) {
        error_report("gvt-stream: release-dmabuf #%" PRIu64 " console=%d "
                     "dmabuf=%p",
                     gdpy->release_count, qemu_console_get_index(dcl->con),
                     dmabuf);
    }
}

static void gvt_stream_gl_update(DisplayChangeListener *dcl,
                                 uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h)
{
    GVTStreamDisplay *gdpy = container_of(dcl, GVTStreamDisplay, dcl);
    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    gdpy->update_count++;
    gdpy->report_updates++;
    gdpy->last_update_ms = now_ms;

    if (!gdpy->last_report_ms) {
        gdpy->last_report_ms = now_ms;
    }

    if (gdpy->verbose) {
        error_report("gvt-stream: gl-update console=%d rect=%u,%u %ux%u "
                     "scanout=%p total=%" PRIu64,
                     qemu_console_get_index(dcl->con), x, y, w, h,
                     gdpy->scanout, gdpy->update_count);
    }

    gvt_stream_capture_frame(gdpy, now_ms);
    gvt_stream_kms_present(gdpy);
    gvt_stream_publish_dmabuf(gdpy, gdpy->scanout, now_ms, false);

    if (now_ms - gdpy->last_report_ms >= gdpy->report_ms) {
        int64_t delta = now_ms - gdpy->last_report_ms;
        double fps = delta > 0 ? (double)gdpy->report_updates * 1000.0 / delta : 0.0;

        error_report("gvt-stream: update-stats console=%d updates=%" PRIu64
                     " interval_ms=%" PRId64 " fps=%.2f last_rect=%u,%u %ux%u "
                     "scanouts=%" PRIu64 " cursor=%" PRIu64 " releases=%" PRIu64
                     " imports=%" PRIu64 " import_failures=%" PRIu64
                     " captures=%" PRIu64 " capture_failures=%" PRIu64
                     " encoded=%" PRIu64 " encode_failures=%" PRIu64
                     " dmabuf=%" PRIu64 " cpu=%" PRIu64 " capture_ms=%" PRIu64
                     " probe_diff_ppm=%" PRIu64 " probes=%" PRIu64
                     " idle_wakes=%" PRIu64
                     " kms=%" PRIu64 "/%" PRIu64
                     " atomic=%" PRIu64 " atomic_fallback=%" PRIu64
                     " pf=%" PRIu64 " ms=%" PRIu64
                     " publish=%" PRIu64 "/%" PRIu64
                     " last_checksum=0x%016" PRIx64,
                     qemu_console_get_index(dcl->con), gdpy->report_updates,
                     delta, fps, x, y, w, h, gdpy->scanout_count,
                     gdpy->cursor_count, gdpy->release_count,
                     gdpy->import_count, gdpy->import_fail_count,
                     gdpy->capture_count, gdpy->capture_fail_count,
                     gdpy->encode_count, gdpy->encode_fail_count,
                     gdpy->encode_dmabuf_count, gdpy->encode_cpu_count,
                     gvt_stream_effective_capture_ms(gdpy, now_ms),
                      gdpy->last_probe_diff_ppm, gdpy->idle_probe_count,
                      gdpy->idle_wake_count,
                      gdpy->kms_commit_count, gdpy->kms_fail_count,
                      gdpy->kms_atomic_count, gdpy->kms_atomic_fallback_count,
                      gdpy->kms_page_flip_count, gdpy->kms_modeset_count,
                      gdpy->publish_count, gdpy->publish_fail_count,
                      gdpy->last_capture_checksum);

        gdpy->last_report_ms = now_ms;
        gdpy->report_updates = 0;
    }
}

static const DisplayChangeListenerOps gvt_stream_ops = {
    .dpy_name               = "gvt-stream",
    .dpy_refresh            = gvt_stream_refresh,
    .dpy_gfx_update         = gvt_stream_gfx_update,
    .dpy_gfx_switch         = gvt_stream_gfx_switch,
    .dpy_gl_scanout_disable = gvt_stream_scanout_disable,
    .dpy_gl_scanout_texture = gvt_stream_scanout_texture,
    .dpy_has_dmabuf         = gvt_stream_has_dmabuf,
    .dpy_gl_scanout_dmabuf  = gvt_stream_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf   = gvt_stream_cursor_dmabuf,
    .dpy_gl_cursor_position = gvt_stream_cursor_position,
    .dpy_gl_release_dmabuf  = gvt_stream_release_dmabuf,
    .dpy_gl_update          = gvt_stream_gl_update,
};

static bool gvt_stream_is_compatible_dcl(DisplayGLCtx *dgc,
                                         DisplayChangeListener *dcl)
{
    return dcl->ops == &gvt_stream_ops;
}

static QEMUGLContext gvt_stream_create_context(DisplayGLCtx *dgc,
                                               QEMUGLParams *params)
{
    return qemu_egl_create_context(dgc, params, qemu_egl_rn_ctx);
}

static const DisplayGLCtxOps gvt_stream_gl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = gvt_stream_is_compatible_dcl,
    .dpy_gl_ctx_create            = gvt_stream_create_context,
    .dpy_gl_ctx_destroy           = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current      = qemu_egl_make_context_current,
};

static void early_gvt_stream_init(DisplayOptions *opts)
{
    DisplayGLMode mode = opts->has_gl ? opts->gl : DISPLAY_GL_MODE_ON;

    egl_init(opts->u.gvt_stream.rendernode, mode, &error_fatal);
}

static void gvt_stream_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con;
    GVTStreamDisplay *gdpy;
    DisplayGLCtx *ctx;
    const char *host = opts->u.gvt_stream.host ?: "";
    const char *codec = opts->u.gvt_stream.codec ?: "diag";
    uint16_t port = opts->u.gvt_stream.has_port ? opts->u.gvt_stream.port : 0;
    int idx;

    error_report("gvt-stream: init host=%s port=%u codec=%s rendernode=%s",
                 host, port, codec, opts->u.gvt_stream.rendernode ?: "auto");

    gvt_stream_input_start();

    for (idx = 0;; idx++) {
        con = qemu_console_lookup_by_index(idx);
        if (!con || !qemu_console_is_graphic(con)) {
            break;
        }

        gdpy = g_new0(GVTStreamDisplay, 1);
        gdpy->kms_fd = -1;
        gdpy->dcl.con = con;
        gdpy->dcl.ops = &gvt_stream_ops;
        gdpy->refresh_ms = gvt_stream_getenv_u64("GVT_STREAM_REFRESH_MS",
                                                 16, 1, 1000);
        gdpy->report_ms = gvt_stream_getenv_u64("GVT_STREAM_REPORT_MS",
                                                1000, 100, 60000);
        gdpy->verbose = gvt_stream_getenv_bool("GVT_STREAM_VERBOSE", false);
        gdpy->import_test = gvt_stream_getenv_bool("GVT_STREAM_IMPORT_TEST", false);
        gdpy->capture_dir = g_strdup(g_getenv("GVT_STREAM_CAPTURE_DIR"));
        if (gdpy->capture_dir && !*gdpy->capture_dir) {
            g_clear_pointer(&gdpy->capture_dir, g_free);
        }
        gdpy->capture_ms = gvt_stream_getenv_u64("GVT_STREAM_CAPTURE_MS",
                                                 1000, 16, 60000);
        gdpy->idle_capture_ms =
            gvt_stream_getenv_u64("GVT_STREAM_IDLE_CAPTURE_MS",
                                  gdpy->capture_ms, 16, 60000);
        gdpy->idle_after_ms =
            gvt_stream_getenv_u64("GVT_STREAM_IDLE_AFTER_MS",
                                  1000, 0, 60000);
        gdpy->idle_probe_ms =
            gvt_stream_getenv_u64("GVT_STREAM_IDLE_PROBE_MS",
                                  250, 0, 60000);
        gdpy->idle_changed_ppm =
            gvt_stream_getenv_u64("GVT_STREAM_IDLE_CHANGED_PPM",
                                  3000, 0, 1000000);
        gdpy->idle_pixel_delta =
            gvt_stream_getenv_u64("GVT_STREAM_IDLE_PIXEL_DELTA",
                                  8, 0, 255);
        gdpy->capture_max = gvt_stream_getenv_u64("GVT_STREAM_CAPTURE_MAX",
                                                  5, 0, 1000000);
        gdpy->encode_file = g_strdup(g_getenv("GVT_STREAM_ENCODE_FILE"));
        if (gdpy->encode_file && !*gdpy->encode_file) {
            g_clear_pointer(&gdpy->encode_file, g_free);
        }
        gdpy->encode_max = gvt_stream_getenv_u64("GVT_STREAM_ENCODE_MAX",
                                                 120, 0, 1000000);
        gdpy->encode_fps = gvt_stream_getenv_u64("GVT_STREAM_ENCODE_FPS",
                                                 30, 1, 120);
        gdpy->encode_bitrate = gvt_stream_getenv_u64("GVT_STREAM_ENCODE_BITRATE",
                                                     18000, 256, 100000);
        gdpy->encode_keyint = gvt_stream_getenv_u64("GVT_STREAM_ENCODE_KEYINT",
                                                    30, 1, 300);
        gdpy->encode_flip = gvt_stream_getenv_bool("GVT_STREAM_ENCODE_FLIP", false);
        gdpy->encode_dmabuf_caps_feature =
            gvt_stream_getenv_bool("GVT_STREAM_DMABUF_CAPS_FEATURE", false);
        {
            const char *path = g_getenv("GVT_STREAM_ENCODE_PATH");
            gdpy->encode_dmabuf = path && !g_ascii_strcasecmp(path, "dmabuf");
        }
        gdpy->rtp_host = g_strdup(g_getenv("GVT_STREAM_RTP_HOST"));
        if (gdpy->rtp_host && !*gdpy->rtp_host) {
            g_clear_pointer(&gdpy->rtp_host, g_free);
        }
        gdpy->publish_socket = g_strdup(g_getenv("GVT_STREAM_PUBLISH_SOCKET"));
        if (gdpy->publish_socket && !*gdpy->publish_socket) {
            g_clear_pointer(&gdpy->publish_socket, g_free);
        }
        gdpy->source_id = g_strdup(g_getenv("GVT_STREAM_SOURCE_ID"));
        if (gdpy->source_id && !*gdpy->source_id) {
            g_clear_pointer(&gdpy->source_id, g_free);
        }
        if (!gvt_stream_input_display) {
            gvt_stream_input_display = gdpy;
        }
        if (!gdpy->rtp_host && host && *host && port) {
            gdpy->rtp_host = g_strdup(host);
        }
        gdpy->rtp_port = gvt_stream_getenv_u64("GVT_STREAM_RTP_PORT",
                                               port, 0, 65535);
        gdpy->rtp_fec = gvt_stream_getenv_u64("GVT_STREAM_RTP_FEC",
                                              0, 0, 100);
        gdpy->rtp_fec_important =
            gvt_stream_getenv_u64("GVT_STREAM_RTP_FEC_IMPORTANT", 0, 0, 100);
        gdpy->kms_connector = g_strdup(g_getenv("GVT_STREAM_KMS_CONNECTOR"));
        if (gdpy->kms_connector && !*gdpy->kms_connector) {
            g_clear_pointer(&gdpy->kms_connector, g_free);
        }
        gdpy->kms_device = g_strdup(g_getenv("GVT_STREAM_KMS_DEVICE"));
        if (gdpy->kms_device && !*gdpy->kms_device) {
            g_clear_pointer(&gdpy->kms_device, g_free);
        }
        gdpy->kms_atomic =
            gvt_stream_getenv_bool("GVT_STREAM_KMS_ATOMIC", true);
        if (gdpy->rtp_host && !gdpy->rtp_port) {
            warn_report("gvt-stream: disabling RTP, missing port for host %s",
                        gdpy->rtp_host);
            g_clear_pointer(&gdpy->rtp_host, g_free);
        }
        if (gdpy->encode_file) {
            g_autofree char *dirname = g_path_get_dirname(gdpy->encode_file);
            if (g_mkdir_with_parents(dirname, 0755) < 0) {
                warn_report("gvt-stream: disabling encode, mkdir %s failed: %s",
                            dirname, strerror(errno));
                g_clear_pointer(&gdpy->encode_file, g_free);
            }
        }
        if (gdpy->capture_dir &&
            g_mkdir_with_parents(gdpy->capture_dir, 0755) < 0) {
            warn_report("gvt-stream: disabling capture, mkdir %s failed: %s",
                        gdpy->capture_dir, strerror(errno));
            g_clear_pointer(&gdpy->capture_dir, g_free);
        }
        gdpy->dcl.update_interval = gdpy->refresh_ms;

        ctx = g_new0(DisplayGLCtx, 1);
        ctx->ops = &gvt_stream_gl_ctx_ops;
        qemu_console_set_display_gl_ctx(con, ctx);

        error_report("gvt-stream: listener console=%d refresh_ms=%" PRIu64
                     " report_ms=%" PRIu64 " verbose=%d import_test=%d "
                     "capture_dir=%s capture_ms=%" PRIu64 " idle_capture_ms=%" PRIu64
                     " idle_after_ms=%" PRIu64 " idle_probe_ms=%" PRIu64
                     " idle_changed_ppm=%" PRIu64 " idle_pixel_delta=%" PRIu64
                     " capture_max=%" PRIu64
                     " encode_file=%s encode_max=%" PRIu64 " encode_fps=%d "
                     "path=%s flip=%d dmabuf_caps=%d rtp=%s:%u kms=%s "
                     "kms_atomic=%d publish=%s source=%s",
                     qemu_console_get_index(con), gdpy->refresh_ms,
                     gdpy->report_ms, gdpy->verbose, gdpy->import_test,
                     gdpy->capture_dir ?: "", gdpy->capture_ms,
                     gdpy->idle_capture_ms, gdpy->idle_after_ms,
                     gdpy->idle_probe_ms, gdpy->idle_changed_ppm,
                     gdpy->idle_pixel_delta, gdpy->capture_max,
                     gdpy->encode_file ?: "",
                     gdpy->encode_max, gdpy->encode_fps,
                     gdpy->encode_dmabuf ? "dmabuf" : "cpu",
                     gdpy->encode_flip, gdpy->encode_dmabuf_caps_feature,
                     gdpy->rtp_host ?: "", (unsigned)gdpy->rtp_port,
                     gdpy->kms_connector ?: "", gdpy->kms_atomic,
                     gdpy->publish_socket ?: "", gdpy->source_id ?: "");
        register_displaychangelistener(&gdpy->dcl);
    }
}

static QemuDisplay qemu_display_gvt_stream = {
    .type       = DISPLAY_TYPE_GVT_STREAM,
    .early_init = early_gvt_stream_init,
    .init       = gvt_stream_init,
};

static void register_gvt_stream(void)
{
    qemu_display_register(&qemu_display_gvt_stream);
}

type_init(register_gvt_stream);

module_dep("ui-opengl");
