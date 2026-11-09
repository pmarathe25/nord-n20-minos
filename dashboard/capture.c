#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
static int open_output(const char *path) { return open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600); }
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: %s OUTPUT.ppm\n", argv[0]); return 1; }
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
    drmModeCrtc *crtc = drmModeGetCrtc(fd, 82);
    if (!crtc) { perror("drmModeGetCrtc"); return 1; }
    drmModeFB *fb = drmModeGetFB(fd, crtc->buffer_id);
    if (!fb) { perror("drmModeGetFB"); return 1; }
    printf("fb=%u %ux%u depth=%u bpp=%u handle=%u pitch=%u\n", fb->fb_id, fb->width, fb->height, fb->depth, fb->bpp, fb->handle, fb->pitch);
    if (!fb->handle || fb->bpp != 32) return 2;
    struct drm_mode_map_dumb arg = { .handle = fb->handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &arg)) { perror("DRM_IOCTL_MODE_MAP_DUMB"); return 1; }
    size_t size = (size_t)fb->pitch * fb->height;
    unsigned char *pixels = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, arg.offset);
    if (pixels == MAP_FAILED) { perror("mmap"); return 1; }
    int output_fd = open_output(argv[1]);
    if (output_fd < 0) { perror("open output"); return 1; }
    FILE *out = fdopen(output_fd, "wb"); if (!out) { perror("fdopen"); close(output_fd); unlink(argv[1]); return 1; }
    int failed = fprintf(out, "P6\n%u %u\n255\n", fb->width, fb->height) < 0;
    uint64_t nonblack = 0, total = 0;
    for (uint32_t y=0; y<fb->height; y++) {
        uint32_t *row = (uint32_t *)(pixels + (size_t)y * fb->pitch);
        for (uint32_t x=0; x<fb->width; x++) { uint32_t pixel=row[x]; unsigned char rgb[3]={(pixel>>16)&255,(pixel>>8)&255,pixel&255}; if (fwrite(rgb,1,3,out)!=3) failed=1; if (pixel & 0xffffff) nonblack++; total++; }
    }
    if (fclose(out)!=0) failed=1;
    if (failed) { unlink(argv[1]); fprintf(stderr,"capture write failed\n"); }
    else printf("nonblack pixels: %" PRIu64 " / %" PRIu64 "\n", nonblack, total);
    munmap(pixels, size); drmModeFreeFB(fb); drmModeFreeCrtc(crtc); close(fd); return failed?1:0;
}
