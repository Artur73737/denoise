#include "app.h"
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const wchar_t *extension(const wchar_t *path) {
    const wchar_t *slash = wcsrchr(path, L'\\'), *forward = wcsrchr(path, L'/'), *dot = wcsrchr(path, L'.');
    if (forward && (!slash || forward > slash)) slash = forward;
    return dot && (!slash || dot > slash) ? dot : L"";
}
static int absolute(const wchar_t *src, wchar_t *dst, DnError *err) {
    DWORD n = GetFullPathNameW(src, DN_PATH, dst, NULL);
    return n && n < DN_PATH ? 1 : dn_error(err, "Invalid or excessively long path");
}
static int output_path(const wchar_t *input, const wchar_t *requested, const wchar_t *ext, int amount,
    wchar_t *output, DnError *err) {
    if (requested) { if (!absolute(requested, output, err)) return 0; }
    else {
        const wchar_t *original_ext = extension(input);
        size_t stem = *original_ext ? (size_t)(original_ext - input) : wcslen(input);
        if (swprintf_s(output, DN_PATH, L"%.*ls_denoised_%d%ls", (int)stem, input, amount, ext) < 0)
            return dn_error(err, "Output path is too long");
    }
    if (_wcsicmp(extension(output), ext) && !(!_wcsicmp(ext, L".tif") && !_wcsicmp(extension(output), L".tiff")))
        return dn_error(err, "Invalid output extension. Developed RAW/codec images require .tif/.tiff; native TIFF/BMP keeps its source extension");
    if (!_wcsicmp(input, output) || dn_same_file(input, output)) return dn_error(err, "The source file cannot be overwritten");
    if (GetFileAttributesW(output) != INVALID_FILE_ATTRIBUTES) return dn_error(err, "Output already exists; choose another filename");
    return 1;
}
int dn_job_run(void *context, DnError *err) {
    DnJob *job = context;
    DnOptions *options = &job->options;
    DnImage image = {0};
    const DnModel *model;
    wchar_t *input = calloc(DN_PATH, sizeof(wchar_t)), *cache = calloc(DN_PATH, sizeof(wchar_t)), *graph = calloc(DN_PATH, sizeof(wchar_t));
    ULONGLONG started = GetTickCount64();
    int ok = 0, com = 0;
    if (!input || !cache || !graph) { dn_error(err, "Out of memory"); goto done; }
    dn_status(DN_DECODE, "Opening source image");
    if (!absolute(options->input, input, err)) goto done;
    if (GetFileAttributesW(input) == INVALID_FILE_ATTRIBUTES) { dn_error(err, "Source file does not exist or cannot be accessed"); goto done; }
    if (!options->amount) {
        if (!output_path(input, options->output, extension(input), 0, job->output, err)) goto done;
        if (!CopyFileW(input, job->output, TRUE)) { dn_error(err, "Exact copy failed (Windows error %lu)", GetLastError()); goto done; }
        dn_status(DN_DONE, "Exact byte-for-byte copy saved: %ls", job->output); ok = 1; goto done;
    }
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) { dn_error(err, "Cannot initialize image services"); goto done; }
    com = 1;
    if (!dn_image_load(input, &image, err) || dn_cancelled()) goto canceled_or_done;
    dn_status(DN_DECODE, "%u x %u %s | %s", image.width, image.height, image.grayscale ? "grayscale" : "RGB", image.decoder);
    if (!output_path(input, options->output, image.storage ? extension(input) : L".tif", options->amount, job->output, err)) goto done;
    model = dn_model_select(image.grayscale, options->amount, options->model);
    if (!model) { dn_error(err, "Unknown model. Open 'denoise.exe model' to see the available IDs"); goto done; }
    if (model->channels == 1 && !image.grayscale) { dn_error(err, "This model requires a grayscale image. Choose a scunet_color_* model to preserve the image colors"); goto done; }
    if (!dn_models_directory(cache, err)) goto done;
    if (!dn_model_ensure(model, cache, options->offline, graph, err)) goto done;
    if (!dn_process(&image, options, model, graph, err)) goto done;
    if (dn_cancelled()) goto canceled_or_done;
    dn_status(DN_SAVE, image.storage ? "Saving image; preserving original container and metadata" : "Saving uncompressed RGB/RGBA16 TIFF; source stays unchanged");
    if (!dn_image_save_new(job->output, &image, err)) goto done;
    dn_status(DN_DONE, "Saved: %ls", job->output);
    ok = 1; goto done;
canceled_or_done:
    if (dn_cancelled()) dn_error(err, "Canceled; source file unchanged");
done:
    job->elapsed = (GetTickCount64() - started) / 1000.0;
    dn_image_free(&image);
    if (com) CoUninitialize();
    free(input); free(cache); free(graph);
    return ok;
}
int dn_install_run(void *context, DnError *err) {
    DnInstall *install = context;
    wchar_t *graph = calloc(DN_PATH, sizeof(wchar_t));
    int ok;
    if (!graph) return dn_error(err, "Out of memory");
    ok = dn_model_ensure(install->model, install->cache, 0, graph, err);
    if (ok) dn_status(DN_DONE, "Model installed and SHA-256 verified");
    free(graph); return ok;
}
void dn_plain_progress(void *context, const DnProgress *p) {
    static DnStage last = (DnStage)-1;
    static char subject[192], message[768];
    static ULONGLONG tick;
    ULONGLONG now = GetTickCount64();
    (void)context;
    if (p->stage != last || strcmp(subject, p->subject) || strcmp(message, p->message) || now - tick >= 1000) {
        fprintf(stderr, "[%s] %s%s%s", dn_stage_name(p->stage), p->subject, p->subject[0] ? ": " : "", p->message);
        if (p->total) fprintf(stderr, " (%llu/%llu)", p->done, p->total);
        fputc('\n', stderr);
        last = p->stage; tick = now;
        strcpy_s(subject, sizeof(subject), p->subject); strcpy_s(message, sizeof(message), p->message);
    }
}
