#include "denoise.h"
#include <libraw/libraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void dn_raw_formats(int cameras) {
    int i;
    printf("LibRaw %s: %d supported camera models listed.\n", libraw_version(), libraw_cameraCount());
    puts("RAW families: CR2, CR3, CRW, DNG, NEF, NRW, ARW, SR2, SRF, RAF, ORF, RW2, RAW, PEF, PTX, SRW, 3FR, FFF, IIQ, MOS, MRW, MEF, ERF, X3F, DCR, KDC and others.\n"
         "Compatibility depends on camera/codec and LibRaw version, not just the extension.\n"
         "Windows codecs: JPEG, PNG, TIFF, BMP, GIF and additional installed codecs.\n"
         "Developed RAW/codec images are saved as uncompressed RGB/RGBA16 TIFF. Source files stay unchanged.");
    if (cameras) {
        const char **list = libraw_cameraList();
        for (i = 0; list && list[i]; ++i) puts(list[i]);
    }
}
int dn_raw_inspect(const wchar_t *path, DnError *err) {
    libraw_data_t *raw = libraw_init(0);
    int rc;
    if (!raw) return dn_error(err, "LibRaw: out of memory");
    rc = libraw_open_wfile(raw, path);
    if (rc) { libraw_close(raw); return dn_error(err, "LibRaw: %s", libraw_strerror(rc)); }
    printf("RAW decoder: LibRaw %s\nCamera: %s %s\nSensor: %u x %u; active area: %u x %u\nMosaic: %s; DNG: %s\nDenoise output: RGB16 TIFF\n",
        libraw_version(), raw->idata.make, raw->idata.model, raw->sizes.raw_width, raw->sizes.raw_height,
        raw->sizes.width, raw->sizes.height, raw->idata.filters == 9 ? "X-Trans" : raw->idata.filters ? "CFA/Bayer" : "linear/multichannel",
        raw->idata.dng_version ? "yes" : "no");
    libraw_close(raw); return 1;
}
static int progress(void *context, enum LibRaw_progress stage, int iteration, int expected) {
    enum LibRaw_progress *last = context;
    (void)iteration; (void)expected;
    if (*last != stage) { dn_status(DN_DECODE, "Developing RAW: %s", libraw_strprogress(stage)); *last = stage; }
    return dn_cancelled();
}
int dn_raw_load(const wchar_t *path, DnImage *image, DnError *err) {
    libraw_data_t *raw = NULL;
    libraw_processed_image_t *pixels = NULL;
    enum LibRaw_progress last = LIBRAW_PROGRESS_START;
    size_t count, i;
    int rc, ok = 0;
    struct tm captured;
    memset(image, 0, sizeof(*image));
    image->source = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (image->source == INVALID_HANDLE_VALUE) return dn_error(err, "Cannot open RAW file (Windows error %lu)", GetLastError());
    raw = libraw_init(0);
    if (!raw) { dn_error(err, "Out of memory for LibRaw"); goto done; }
    rc = libraw_open_wfile(raw, path);
    if (rc) { dn_error(err, "LibRaw: %s", libraw_strerror(rc)); goto done; }
    dn_status(DN_DECODE, "%s %s (%u x %u)", raw->idata.make, raw->idata.model, raw->sizes.width, raw->sizes.height);
    if (!raw->sizes.width || !raw->sizes.height || (uint64_t)raw->sizes.width * raw->sizes.height > 200000000) {
        dn_error(err, "RAW image exceeds the 200-megapixel limit"); goto done;
    }
    libraw_set_progress_handler(raw, progress, &last);
    raw->rawparams.max_raw_memory_mb = 2048;
    raw->params.output_bps = 16;
    raw->params.output_color = 1; /* sRGB, high precision all the way to the model. */
    raw->params.use_camera_wb = 1;
    raw->params.no_auto_bright = 1;
    raw->params.user_qual = 3;
    raw->params.gamm[0] = 1.0 / 2.4;
    raw->params.gamm[1] = 12.92;
    rc = libraw_unpack(raw);
    if (!rc) rc = libraw_dcraw_process(raw);
    if (rc) { dn_error(err, dn_cancelled() ? "Canceled" : "RAW development failed: %s", libraw_strerror(rc)); goto done; }
    pixels = libraw_dcraw_make_mem_image(raw, &rc);
    if (!pixels || rc || pixels->type != LIBRAW_IMAGE_BITMAP || pixels->bits != 16 ||
        (pixels->colors != 1 && pixels->colors != 3) || !pixels->width || !pixels->height) {
        dn_error(err, "LibRaw did not produce a valid 16-bit RGB/grayscale image (%d)", rc); goto done;
    }
    count = (size_t)pixels->width * pixels->height;
    if (count > 200000000 || count * pixels->colors * 2 > pixels->data_size) { dn_error(err, "Invalid decoded RAW bitmap"); goto done; }
    image->rgba = malloc(count * 4 * sizeof(float));
    if (!image->rgba) { dn_error(err, "Out of memory for RAW pixels"); goto done; }
    image->width = pixels->width; image->height = pixels->height;
    image->grayscale = pixels->colors == 1; image->is_raw = 1;
    strcpy_s(image->decoder, sizeof(image->decoder), "LibRaw " LIBRAW_VERSION_STR);
    for (i = 0; i < count; ++i) {
        const uint16_t *p = (const uint16_t *)pixels->data + i * pixels->colors;
        int c;
        for (c = 0; c < 3; ++c) image->rgba[i * 4 + c] = p[pixels->colors == 1 ? 0 : c] / 65535.0f;
        image->rgba[i * 4 + 3] = 1.0f;
    }
    strncpy_s(image->metadata.make, sizeof(image->metadata.make), raw->idata.make, _TRUNCATE);
    strncpy_s(image->metadata.model, sizeof(image->metadata.model), raw->idata.model, _TRUNCATE);
    strncpy_s(image->metadata.artist, sizeof(image->metadata.artist), raw->other.artist, _TRUNCATE);
    image->metadata.exposure = raw->other.shutter;
    image->metadata.aperture = raw->other.aperture;
    image->metadata.focal_length = raw->other.focal_len;
    image->metadata.iso = raw->other.iso_speed;
    if (raw->other.timestamp > 0 && !localtime_s(&captured, &raw->other.timestamp))
        strftime(image->metadata.datetime, sizeof(image->metadata.datetime), "%Y:%m:%d %H:%M:%S", &captured);
    dn_set_srgb_profile(image);
    ok = 1;
done:
    if (pixels) libraw_dcraw_clear_mem(pixels);
    if (raw) libraw_close(raw);
    if (!ok) dn_image_free(image);
    return ok;
}
