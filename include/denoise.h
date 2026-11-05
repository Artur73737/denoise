#ifndef PHOTO_DENOISE_H
#define PHOTO_DENOISE_H
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#define DN_PATH 32768
typedef struct { char message[2048]; } DnError;
typedef struct DnStorage DnStorage;
typedef struct {
    char make[128], model[128], artist[128], datetime[32];
    float exposure, aperture, focal_length, iso;
    unsigned orientation;
    double xdpi, ydpi;
    unsigned char *icc;
    UINT icc_size;
} DnMetadata;
typedef struct {
    UINT width, height;
    float *rgba;
    int grayscale;
    HANDLE source;
    DnStorage *storage;
    int is_raw, has_alpha;
    char decoder[64];
    DnMetadata metadata;
} DnImage;
typedef struct {
    const char *id;
    int channels;
    const char *graph_hash;
    const char *weights_hash;
    size_t graph_bytes;
    const char *title, *description, *performance;
} DnModel;

typedef enum { DN_IDLE, DN_DECODE, DN_VERIFY, DN_DOWNLOAD, DN_SESSION, DN_INFERENCE, DN_SAVE, DN_DONE } DnStage;
typedef struct {
    DnStage stage;
    uint64_t done, total;
    char subject[192], message[768];
} DnProgress;
typedef void (*DnProgressFn)(void *context, const DnProgress *progress);
void dn_set_progress(DnProgressFn callback, void *context);
void dn_progress(DnStage stage, uint64_t done, uint64_t total, const char *subject, const char *message);
void dn_status(DnStage stage, const char *fmt, ...);
void dn_cancel_reset(void);
void dn_cancel(void);
int dn_cancelled(void);
const char *dn_stage_name(DnStage stage);
int dn_models_directory(wchar_t *path, DnError *err);
size_t dn_model_count(void);
const DnModel *dn_model_at(size_t index);
const DnModel *dn_model_find(const char *id);
int dn_model_ready(const DnModel *model, const wchar_t *cache);
uint64_t dn_model_size(const DnModel *model);
typedef struct {
    const wchar_t *input, *output, *cache;
    int amount, cpu, device, tile, offline;
    const char *model;
} DnOptions;
typedef struct DnBackend DnBackend;

int dn_error(DnError *err, const char *fmt, ...);
int dn_hresult(DnError *err, const char *op, HRESULT hr);
int dn_path_join(wchar_t *out, size_t capacity, const wchar_t *a, const wchar_t *b, DnError *err);
int dn_mkdir(const wchar_t *path, DnError *err);
int dn_reflect(int x, int size);
int dn_same_file(const wchar_t *a, const wchar_t *b);
int dn_image_load(const wchar_t *path, DnImage *image, DnError *err);
int dn_native_load(const wchar_t *path, DnImage *image, DnError *err);
int dn_raw_load(const wchar_t *path, DnImage *image, DnError *err);
int dn_wic_load(const wchar_t *path, DnImage *image, DnError *err);
int dn_tiff_save_new(const wchar_t *path, const DnImage *image, DnError *err);
int dn_raw_inspect(const wchar_t *path, DnError *err);
void dn_raw_formats(int cameras);
void dn_set_srgb_profile(DnImage *image);
int dn_image_save_new(const wchar_t *path, const DnImage *image, DnError *err);
int dn_storage_open(const wchar_t *path, DnImage *image, DnError *err);
void dn_storage_close(DnImage *image);
void dn_image_free(DnImage *image);
const DnModel *dn_model_select(int grayscale, int amount, const char *override);
int dn_model_ensure(const DnModel *model, const wchar_t *cache, int offline, wchar_t *graph, DnError *err);
int dn_file_verify(const wchar_t *path, const char *hash, size_t expected_bytes);
int dn_download(const wchar_t *url, const wchar_t *path, const char *hash, size_t expected_bytes, DnError *err);
int dn_gpu_list(void);
int dn_backend_open(DnBackend **out, const wchar_t *model, int channels, int tile, int cpu, int device, DnError *err);
int dn_backend_run(DnBackend *backend, float *input, float *output, DnError *err);
void dn_backend_close(DnBackend *backend);
const char *dn_backend_name(const DnBackend *backend);
int dn_process(DnImage *image, const DnOptions *options, const DnModel *model, const wchar_t *graph, DnError *err);
#endif
