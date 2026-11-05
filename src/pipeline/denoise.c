#include "denoise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int process_tiles(DnImage *image, DnBackend *backend, const DnOptions *options,
    int channels, float *result, DnError *err) {
    const int tile = options->tile, halo = 32, core = tile - halo * 2;
    const size_t plane = (size_t)tile * tile;
    float *input = malloc(plane * channels * sizeof(float));
    float *output = malloc(plane * channels * sizeof(float));
    UINT tx, ty;
    size_t done = 0, total = ((image->width + core - 1) / core) * (size_t)((image->height + core - 1) / core);
    int ok = 0;
    if (!input || !output) { dn_error(err, "Out of memory for image tiles"); goto finish; }
    for (ty = 0; ty < image->height; ty += core) {
        for (tx = 0; tx < image->width; tx += core) {
            int x, y, c;
            if (dn_cancelled()) { dn_error(err, "Canceled"); goto finish; }
            for (y = 0; y < tile; ++y) {
                int sy = dn_reflect((int)ty + y - halo, (int)image->height);
                for (x = 0; x < tile; ++x) {
                    int sx = dn_reflect((int)tx + x - halo, (int)image->width);
                    size_t src = ((size_t)sy * image->width + sx) * 4;
                    for (c = 0; c < channels; ++c)
                        input[c * plane + (size_t)y * tile + x] = image->rgba[src + c];
                }
            }
            if (!dn_backend_run(backend, input, output, err)) goto finish;
            for (y = 0; y < core && ty + y < image->height; ++y) {
                for (x = 0; x < core && tx + x < image->width; ++x) {
                    size_t dst = ((size_t)(ty + y) * image->width + tx + x) * 3;
                    size_t src = (size_t)(y + halo) * tile + x + halo;
                    for (c = 0; c < 3; ++c)
                        result[dst + c] = output[(channels == 1 ? 0 : c) * plane + src];
                }
            }
            dn_progress(DN_INFERENCE, ++done, total, dn_backend_name(backend), "Denoising image tiles");
        }
    }
    ok = 1;
finish:
    free(input); free(output);
    return ok;
}
int dn_process(DnImage *image, const DnOptions *options, const DnModel *model, const wchar_t *graph, DnError *err) {
    DnBackend *backend = NULL;
    const size_t pixels = (size_t)image->width * image->height;
    float *result = NULL;
    size_t i;
    int ok = 0;
    float alpha = options->amount / 100.0f;
    if (!options->amount) return 1;
    result = malloc(pixels * 3 * sizeof(float));
    if (!result) return dn_error(err, "Out of memory for the output image");
    if (!dn_backend_open(&backend, graph, model->channels, options->tile, options->cpu, options->device, err)) goto finish;
    dn_status(DN_INFERENCE, "Backend: %s | Model: %s | Strength: %d", dn_backend_name(backend), model->id, options->amount);
    if (!process_tiles(image, backend, options, model->channels, result, err)) {
        if (!dn_cancelled() && !strncmp(dn_backend_name(backend), "DirectML", 8)) {
            dn_status(DN_SESSION, "GPU inference failed: %s. Retrying the full image on CPU", err->message);
            dn_backend_close(backend); backend = NULL;
            if (!dn_backend_open(&backend, graph, model->channels, options->tile, 1, -1, err) ||
                !process_tiles(image, backend, options, model->channels, result, err)) goto finish;
        } else goto finish;
    }
    /* Commit only after every tile succeeded. The alpha channel stays untouched. */
    for (i = 0; i < pixels; ++i) {
        int c;
        for (c = 0; c < 3; ++c) {
            float v = image->rgba[i * 4 + c] * (1.0f - alpha) + result[i * 3 + c] * alpha;
            image->rgba[i * 4 + c] = fmaxf(0.0f, fminf(1.0f, v));
        }
    }
    ok = 1;
finish:
    dn_backend_close(backend); free(result);
    return ok;
}
