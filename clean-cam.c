// clean-cam: on-demand temporal denoiser for the Asahi FaceTime HD camera.
//
// Reads NV12 frames from the real camera (apple-isp), runs a motion-adaptive
// temporal noise filter on them and writes them to a v4l2loopback device.
// The real camera is only opened while some app is streaming from the
// loopback device; otherwise the process sleeps in poll() and uses no CPU.
//
// Build: make      Run: clean-cam [options]      Help: clean-cam -h

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// From v4l2loopback.h (0.13+): sent when a reader starts/stops streaming.
#define V4L2_EVENT_PRI_CLIENT_USAGE (V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1)

#define NBUF 4

static struct {
    const char *src;      // real camera; NULL = find by card name
    const char *dst;      // v4l2loopback device
    int w, h, fps;
    int strength;         // 0 = passthrough .. 10 = very strong
    int grace_ms;         // keep camera on this long after the last reader leaves
    int dynamic_fps;      // let the ISP drop fps in low light (longer exposure)
    int verbose;
    int thresh_y, thresh_uv;  // motion thresholds in 8-bit levels
} opt = {NULL, "/dev/video42", 1920, 1080, 30, 5, 3000, 0, 0, 20, 24};

static volatile sig_atomic_t quit;
static void on_signal(int s) { (void)s; quit = 1; }

#define LOG(...) do { fprintf(stderr, "clean-cam: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VLOG(...) do { if (opt.verbose) LOG(__VA_ARGS__); } while (0)

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// ---------------------------------------------------------------- denoiser
//
// Per pixel: acc (8.8 fixed point) moves toward the new sample by a factor
// alpha. Where the change is small (noise) alpha is small, so the pixel is
// averaged over many frames; where it is large (motion) alpha goes to 1 and
// the new value is taken as is, so moving things don't smear.

#define TNR_Y  tnr_params(opt.strength, opt.thresh_y)
#define TNR_UV tnr_params(opt.strength + 2, opt.thresh_uv)

struct tnr { int amin, k, qmax; };   // alpha in Q8 (256 = 1.0)

// alpha = amin + (1 - amin) * (|diff| / thresh)^2, capped at 1. The square keeps
// alpha low for noise-sized differences and ramps up fast for real motion.
static struct tnr tnr_params(int strength, int thresh)
{
    int amin = 256 / (1 + strength);
    return (struct tnr){amin, ((256 - amin) << 8) / (thresh * thresh), thresh << 4};
}

static void tnr_row(const uint8_t *restrict in, uint16_t *restrict acc,
                    uint8_t *restrict out, int n, struct tnr p)
{
    for (int i = 0; i < n; i++) {
        int32_t d = (in[i] << 8) - acc[i];
        int32_t q = (d < 0 ? -d : d) >> 4;           // |diff| in Q4
        q = q > p.qmax ? p.qmax : q;
        int32_t a = p.amin + ((q * q * p.k) >> 16);
        a = a > 256 ? 256 : a;
        int32_t v = acc[i] + ((d * a) >> 8);
        acc[i] = (uint16_t)v;
        out[i] = (uint8_t)((v + 128) >> 8);
    }
}

struct filter {
    int w, h;
    uint16_t *acc;        // w*h*3/2
    int primed;
    struct tnr y, uv;
};

static void filter_reset(struct filter *f) { f->primed = 0; }

// src: NV12 with given stride and UV plane offset; dst: packed NV12 (w*h*3/2)
static void filter_frame(struct filter *f, const uint8_t *src, int stride,
                         size_t uv_off, uint8_t *dst)
{
    int w = f->w, h = f->h;
    if (!f->primed || opt.strength == 0) {
        for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, src + (size_t)y * stride, w);
        for (int y = 0; y < h / 2; y++)
            memcpy(dst + (size_t)w * h + (size_t)y * w, src + uv_off + (size_t)y * stride, w);
        size_t n = (size_t)w * h * 3 / 2;
        for (size_t i = 0; i < n; i++) f->acc[i] = dst[i] << 8;
        f->primed = 1;
        return;
    }
    for (int y = 0; y < h; y++)
        tnr_row(src + (size_t)y * stride, f->acc + (size_t)y * w, dst + (size_t)y * w, w, f->y);
    size_t o = (size_t)w * h;
    for (int y = 0; y < h / 2; y++)
        tnr_row(src + uv_off + (size_t)y * stride, f->acc + o + (size_t)y * w,
                dst + o + (size_t)y * w, w, f->uv);
}

// ----------------------------------------------------------------- camera

struct cam {
    int fd;
    void *buf[NBUF];
    size_t len[NBUF];
    int stride;
    size_t uv_off;
};

static const char *find_camera(void)
{
    static char path[64];
    DIR *d = opendir("/sys/class/video4linux");
    if (!d) return NULL;
    struct dirent *e;
    const char *found = NULL;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "video", 5)) continue;
        char p[300], name[64] = "";
        snprintf(p, sizeof p, "/sys/class/video4linux/%s/name", e->d_name);
        FILE *fp = fopen(p, "r");
        if (!fp) continue;
        if (fgets(name, sizeof name, fp) && !strncmp(name, "apple-isp", 9)) {
            snprintf(path, sizeof path, "/dev/%s", e->d_name);
            found = path;
        }
        fclose(fp);
        if (found) break;
    }
    closedir(d);
    return found;
}

static void cam_close(struct cam *c)
{
    if (c->fd < 0) return;
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(c->fd, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < NBUF; i++)
        if (c->buf[i]) munmap(c->buf[i], c->len[i]), c->buf[i] = NULL;
    close(c->fd);
    c->fd = -1;
}

static int cam_open(struct cam *c)
{
    const char *path = opt.src ? opt.src : find_camera();
    if (!path) { LOG("apple-isp camera not found"); return -1; }
    c->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (c->fd < 0) { LOG("open %s: %s", path, strerror(errno)); return -1; }

    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    f.fmt.pix.width = opt.w;
    f.fmt.pix.height = opt.h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(c->fd, VIDIOC_S_FMT, &f) < 0) { LOG("camera S_FMT: %s", strerror(errno)); goto fail; }
    if ((int)f.fmt.pix.width != opt.w || (int)f.fmt.pix.height != opt.h ||
        f.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12) {
        LOG("camera gave %ux%u, wanted %dx%d NV12", f.fmt.pix.width, f.fmt.pix.height, opt.w, opt.h);
        goto fail;
    }
    c->stride = f.fmt.pix.bytesperline ? (int)f.fmt.pix.bytesperline : opt.w;
    c->uv_off = (size_t)c->stride * opt.h;
    if (f.fmt.pix.sizeimage < c->uv_off + (size_t)c->stride * opt.h / 2) {
        LOG("camera sizeimage %u too small", f.fmt.pix.sizeimage);
        goto fail;
    }

    struct v4l2_streamparm sp = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    sp.parm.capture.timeperframe = (struct v4l2_fract){1, opt.fps};
    xioctl(c->fd, VIDIOC_S_PARM, &sp);

    struct v4l2_control ctl = {.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY, .value = opt.dynamic_fps};
    xioctl(c->fd, VIDIOC_S_CTRL, &ctl);

    struct v4l2_requestbuffers rb = {.count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                     .memory = V4L2_MEMORY_MMAP};
    if (xioctl(c->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        LOG("camera REQBUFS: %s", strerror(errno));
        goto fail;
    }
    for (unsigned i = 0; i < rb.count && i < NBUF; i++) {
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i};
        if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) goto fail;
        c->len[i] = b.length;
        c->buf[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, c->fd, b.m.offset);
        if (c->buf[i] == MAP_FAILED) { c->buf[i] = NULL; goto fail; }
        if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0) goto fail;
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(c->fd, VIDIOC_STREAMON, &t) < 0) { LOG("camera STREAMON: %s", strerror(errno)); goto fail; }
    LOG("camera on (%s %dx%d@%d)", path, opt.w, opt.h, opt.fps);
    return 0;
fail:
    cam_close(c);
    return -1;
}

// --------------------------------------------------------------- loopback

static int out_open(size_t frame_size)
{
    int fd = open(opt.dst, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { LOG("open %s: %s (is v4l2loopback loaded?)", opt.dst, strerror(errno)); return -1; }

    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    f.fmt.pix.width = opt.w;
    f.fmt.pix.height = opt.h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    f.fmt.pix.bytesperline = opt.w;
    f.fmt.pix.sizeimage = frame_size;
    f.fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
    if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) { LOG("loopback S_FMT: %s", strerror(errno)); close(fd); return -1; }

    struct v4l2_streamparm sp = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
    sp.parm.output.timeperframe = (struct v4l2_fract){1, opt.fps};
    xioctl(fd, VIDIOC_S_PARM, &sp);

    struct v4l2_event_subscription sub = {.type = V4L2_EVENT_PRI_CLIENT_USAGE};
    if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
        LOG("loopback event subscribe: %s (needs v4l2loopback >= 0.13)", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void black(uint8_t *p, int w, int h)
{
    memset(p, 16, (size_t)w * h);
    memset(p + (size_t)w * h, 128, (size_t)w * h / 2);
}

static void put_frame(int fd, const uint8_t *p, size_t n)
{
    if (write(fd, p, n) < 0 && errno != EAGAIN) VLOG("loopback write: %s", strerror(errno));
}

// ------------------------------------------------------------------ bench

static int bench(void)
{
    int w = opt.w, h = opt.h, n = 300;
    size_t sz = (size_t)w * h * 3 / 2;
    uint8_t *clean = malloc(sz), *noisy = malloc(sz), *out = malloc(sz);
    struct filter f = {w, h, malloc(sz * 2), 0, TNR_Y, TNR_UV};
    if (!clean || !noisy || !out || !f.acc) return 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) clean[y * w + x] = 40 + (x * 150 / w) + (y * 30 / h);
    memset(clean + (size_t)w * h, 128, sz - (size_t)w * h);
    uint32_t r = 1;
    double err_in = 0, err_out = 0, t = 0;
    for (int k = 0; k < n; k++) {
        for (size_t i = 0; i < sz; i++) {   // ~uniform noise, sigma ~6
            r = r * 1664525u + 1013904223u;
            int v = clean[i] + (int)((r >> 24) % 21) - 10;
            noisy[i] = v < 0 ? 0 : v > 255 ? 255 : v;
        }
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        filter_frame(&f, noisy, w, (size_t)w * h, out);
        clock_gettime(CLOCK_MONOTONIC, &b);
        t += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        if (k >= n - 30)
            for (size_t i = 0; i < (size_t)w * h; i++) {
                double di = noisy[i] - clean[i], dd = out[i] - clean[i];
                err_in += di * di, err_out += dd * dd;
            }
    }
    printf("%dx%d strength %d: %.2f ms/frame (%.1f%% of one core at %d fps), noise %.2f -> %.2f\n",
           w, h, opt.strength, t / n, t / n * opt.fps / 10, opt.fps,
           __builtin_sqrt(err_in / (30.0 * w * h)), __builtin_sqrt(err_out / (30.0 * w * h)));
    return 0;
}

// ------------------------------------------------------------------- main

static void usage(void)
{
    fprintf(stderr,
        "usage: clean-cam [options]\n"
        "  -i DEV    real camera (default: find the apple-isp camera)\n"
        "  -o DEV    v4l2loopback device (default /dev/video42)\n"
        "  -s WxH    size: 1920x1080 (default), 1280x720, 640x480 ...\n"
        "  -f FPS    frame rate (default 30)\n"
        "  -n N      denoise strength 0-10 (default 5, 0 = off)\n"
        "  -t Y[,UV] motion threshold in levels (default 20,24): higher = smoother, more ghosting\n"
        "  -g MS     keep camera on MS after the last app stops (default 3000)\n"
        "  -d        let the camera lower its frame rate in low light\n"
        "  -b        benchmark the filter and exit\n"
        "  -v        verbose\n");
}

int main(int argc, char **argv)
{
    int c, do_bench = 0;
    while ((c = getopt(argc, argv, "i:o:s:f:n:g:t:dbvh")) != -1) {
        switch (c) {
        case 'i': opt.src = optarg; break;
        case 'o': opt.dst = optarg; break;
        case 's': if (sscanf(optarg, "%dx%d", &opt.w, &opt.h) != 2) { usage(); return 2; } break;
        case 'f': opt.fps = atoi(optarg); break;
        case 'n': opt.strength = atoi(optarg); break;
        case 'g': opt.grace_ms = atoi(optarg); break;
        case 't': if (sscanf(optarg, "%d,%d", &opt.thresh_y, &opt.thresh_uv) < 1) { usage(); return 2; } break;
        case 'd': opt.dynamic_fps = 1; break;
        case 'b': do_bench = 1; break;
        case 'v': opt.verbose = 1; break;
        default: usage(); return c == 'h' ? 0 : 2;
        }
    }
    if (opt.w <= 0 || opt.h <= 0 || (opt.w | opt.h) & 1 || opt.fps <= 0 ||
        opt.strength < 0 || opt.strength > 10) { usage(); return 2; }
    if (do_bench) return bench();

    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    size_t sz = (size_t)opt.w * opt.h * 3 / 2;
    uint8_t *frame = malloc(sz);
    struct filter flt = {opt.w, opt.h, malloc(sz * sizeof(uint16_t)), 0,
                         TNR_Y, TNR_UV};
    if (!frame || !flt.acc) { LOG("out of memory"); return 1; }

    int out = out_open(sz);
    if (out < 0) return 1;
    // One frame makes the loopback device show up as a camera to apps.
    black(frame, opt.w, opt.h);
    put_frame(out, frame, sz);
    LOG("ready on %s (%dx%d@%d, strength %d)", opt.dst, opt.w, opt.h, opt.fps, opt.strength);

    struct cam cam = {.fd = -1};
    int readers = 0;
    int64_t stop_at = -1, retry_at = -1;

    while (!quit) {
        struct pollfd p[2] = {{out, POLLPRI, 0}, {cam.fd, POLLIN, 0}};
        int timeout = -1;
        int64_t t = now_ms();
        if (stop_at >= 0) timeout = stop_at > t ? (int)(stop_at - t) : 0;
        if (retry_at >= 0) { int r = retry_at > t ? (int)(retry_at - t) : 0; timeout = timeout < 0 || r < timeout ? r : timeout; }
        int n = poll(p, cam.fd >= 0 ? 2 : 1, timeout);
        if (n < 0) { if (errno == EINTR) continue; LOG("poll: %s", strerror(errno)); break; }
        t = now_ms();

        if (p[0].revents & POLLPRI) {
            struct v4l2_event ev;
            while (xioctl(out, VIDIOC_DQEVENT, &ev) == 0) {
                if (ev.type != V4L2_EVENT_PRI_CLIENT_USAGE) continue;
                uint32_t count;
                memcpy(&count, ev.u.data, sizeof count);
                readers = count > 0;
                VLOG("readers: %u", count);
                if (readers) {
                    stop_at = -1;
                    if (cam.fd < 0) retry_at = t;
                } else if (cam.fd >= 0) {
                    stop_at = t + opt.grace_ms;
                } else {
                    retry_at = -1;
                }
            }
        }

        if (retry_at >= 0 && t >= retry_at) {
            if (cam_open(&cam) == 0) { retry_at = -1; filter_reset(&flt); }
            else retry_at = t + 2000;   // busy (another app has it?) - try again
        }

        if (stop_at >= 0 && t >= stop_at) {
            cam_close(&cam);
            stop_at = -1;
            black(frame, opt.w, opt.h);   // don't leave the last image behind
            put_frame(out, frame, sz);
            LOG("camera off");
        }

        if (cam.fd >= 0 && (p[1].revents & (POLLERR | POLLHUP))) {
            LOG("camera error, reopening");
            cam_close(&cam);
            if (readers) retry_at = t + 1000;
        } else if (cam.fd >= 0 && (p[1].revents & POLLIN)) {
            struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
            if (xioctl(cam.fd, VIDIOC_DQBUF, &b) == 0) {
                if (!(b.flags & V4L2_BUF_FLAG_ERROR) && (!b.bytesused || b.bytesused >= cam.uv_off) + (size_t)cam.stride * opt.h / 2) {
                    filter_frame(&flt, cam.buf[b.index], cam.stride, cam.uv_off, frame);
                    put_frame(out, frame, sz);
                }
                xioctl(cam.fd, VIDIOC_QBUF, &b);
            }
        }
    }

    cam_close(&cam);
    black(frame, opt.w, opt.h);
    put_frame(out, frame, sz);
    close(out);
    return 0;
}
