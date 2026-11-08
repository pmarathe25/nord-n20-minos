#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
int main(void) {
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
    FILE *out = fopen("/tmp/drm-frame.ppm", "wb"); if (!out) { perror("fopen"); return 1; } fprintf(out, "P6\n%u %u\n255\n", fb->width, fb->height); uint64_t nonblack = 0, total = 0;
    for (uint32_t y=0; y<fb->height; y++) {
        uint32_t *row = (uint32_t *)(pixels + (size_t)y * fb->pitch);
        for (uint32_t x=0; x<fb->width; x++) { uint32_t pixel=row[x]; unsigned char rgb[3]={(pixel>>16)&255,(pixel>>8)&255,pixel&255}; fwrite(rgb,1,3,out); if (pixel & 0xffffff) nonblack++; total++; }
    }
    fclose(out); printf("nonblack pixels: %" PRIu64 " / %" PRIu64 "\n", nonblack, total);
    munmap(pixels, size); drmModeFreeFB(fb); drmModeFreeCrtc(crtc); close(fd); return 0;
}
