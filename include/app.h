#ifndef DENOISE_APP_H
#define DENOISE_APP_H
#include "denoise.h"

typedef int (*DnWork)(void *context, DnError *err);
typedef struct {
    DnOptions options;
    wchar_t output[DN_PATH];
    double elapsed;
} DnJob;
typedef struct { const DnModel *model; wchar_t cache[DN_PATH]; } DnInstall;

int dn_job_run(void *context, DnError *err);
int dn_install_run(void *context, DnError *err);
int dn_tui_available(void);
int dn_tui_models(DnError *err);
int dn_tui_catalog_refresh(DnError *err);
int dn_tui_form(wchar_t *path, size_t capacity, int *strength, DnError *err);
int dn_tui_run(const char *title, const wchar_t *subtitle, DnWork work, void *context, DnError *err);
int dn_tui_job(DnJob *job, DnError *err);
void dn_plain_progress(void *context, const DnProgress *progress);
#endif
