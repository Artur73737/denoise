#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <locale.h>
#include <errno.h>

static void help(void) {
    puts("PHOTO DENOISE 1.2 - C / DirectML / CPU\n\n"
         "Usage: denoise.exe <image> <0..100> [options]\n"
         "       denoise.exe model                  Interactive model library\n"
         "       denoise.exe model list             List model IDs and installed status\n"
         "       denoise.exe model install <id>     Install/repair a specific model\n\n"
         "  --model <id>       Catalog ID, e.g. scunet_color_50 or scunet_color_real_psnr\n"
         "                     Short aliases: auto (default), real, 15, 25, 50\n"
         "  --output <file>    New output file; developed RAW/codec images use .tif/.tiff\n"
         "  --cpu              Force CPU; otherwise try DirectML GPUs, then CPU\n"
         "  --device <id>      Select a DXGI GPU index\n"
         "  --tile <size>      128..512, multiple of 64; default 256\n"
         "  --offline          Require a complete, SHA-256 verified local model\n"
         "  --no-tui           Plain progress for scripts; automatic when redirected\n"
         "  --list-devices     List graphics adapters\n"
         "  --formats          List supported image/RAW families\n"
         "  --list-cameras     List LibRaw camera models\n"
         "  --inspect <file>   Identify a RAW without processing it\n"
         "  --help\n\n"
         "Models are ALWAYS stored in the models folder beside this executable.\n"
         "Interactive terminals show a full-screen UI; no scrolling progress dumps.\n"
         "Strength 0 makes an exact copy. Automatic strength 67..100 selects sigma50.\n"
         "RAW is developed to RGB16 TIFF; source files stay unchanged.\n"
         "Proprietary Adobe .data containers are not used.");
}
static int integer(const wchar_t *s, int min, int max, int *out) {
    wchar_t *end; long n;
    if (!s || !*s) return 0;
    errno = 0; n = wcstol(s, &end, 10);
    if (errno == ERANGE || *end || n < min || n > max) return 0;
    *out = (int)n; return 1;
}
static int model_id(const wchar_t *value, char *out, size_t capacity) {
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, out, (int)capacity, NULL, NULL) != 0;
}
static int model_command(int argc, wchar_t **argv, DnError *err) {
    DnInstall *install = calloc(1, sizeof(*install));
    size_t i;
    int ok = 0, plain = 0;
    char id[128];
    if (!install) return dn_error(err, "Out of memory");
    if (!dn_models_directory(install->cache, err)) goto done;
    if (argc == 2 || (argc == 3 && !wcscmp(argv[2], L"."))) { ok = dn_tui_models(err); goto done; }
    if (argc == 3 && !wcscmp(argv[2], L"list")) {
        puts("MODEL ID                       SIZE (MiB)   STATUS");
        for (i = 0; i < dn_model_count(); ++i) {
            const DnModel *m = dn_model_at(i);
            printf("%-30s %8.2f   %s\n", m->id, dn_model_size(m) / 1048576.0, dn_model_ready(m, install->cache) ? "Installed (verified)" : "Not ready");
        }
        fwprintf(stdout, L"\nInstall directory: %ls\n", install->cache); ok = 1; goto done;
    }
    if ((argc == 4 || argc == 5) && !wcscmp(argv[2], L"install")) {
        if (argc == 5 && wcscmp(argv[4], L"--no-tui")) { dn_error(err, "Unknown model install option"); goto done; }
        plain = argc == 5 || !dn_tui_available();
        if (!model_id(argv[3], id, sizeof(id))) { dn_error(err, "Invalid model ID"); goto done; }
        install->model = dn_model_find(id);
        if (!install->model) { dn_error(err, "Unknown catalog model ID. Run 'denoise.exe model list'"); goto done; }
        if (!plain) ok = dn_tui_run("INSTALL MODEL", argv[3], dn_install_run, install, err);
        else {
            dn_cancel_reset(); dn_set_progress(dn_plain_progress, NULL);
            ok = dn_install_run(install, err); dn_set_progress(NULL, NULL);
            if (ok) printf("Installed and verified: %s\n", install->model->id);
        }
        goto done;
    }
    dn_error(err, "Usage: denoise.exe model [list | install <catalog-id> [--no-tui]]");
done:
    free(install); return ok;
}
int wmain(int argc, wchar_t **argv) {
    DnJob *job = calloc(1, sizeof(*job));
    DnError err = {{0}};
    wchar_t *interactive = calloc(DN_PATH, sizeof(wchar_t));
    char selected[128] = "auto";
    int i, positional = 0, result = 1, no_tui = 0, used_tui = 0;
    setlocale(LC_ALL, ".UTF-8");
    if (!job || !interactive) { dn_error(&err, "Out of memory"); goto done; }
    job->options.amount = -1; job->options.tile = 256; job->options.device = -1; job->options.model = selected;
    if (argc > 1 && !wcscmp(argv[1], L"model")) {
        result = model_command(argc, argv, &err) ? 0 : 1;
        goto done;
    }
    if (argc == 1) {
        job->options.amount = 50;
        if (!dn_tui_form(interactive, DN_PATH, &job->options.amount, &err)) { result = err.message[0] ? 1 : 0; goto done; }
        job->options.input = interactive;
    }
    for (i = 1; i < argc; ++i) {
        const wchar_t *arg = argv[i], *value;
        if (!wcscmp(arg, L"--help") || !wcscmp(arg, L"-h")) { help(); result = 0; goto done; }
        if (!wcscmp(arg, L"--list-devices")) { dn_gpu_list(); result = 0; goto done; }
        if (!wcscmp(arg, L"--formats")) { dn_raw_formats(0); result = 0; goto done; }
        if (!wcscmp(arg, L"--list-cameras")) { dn_raw_formats(1); result = 0; goto done; }
        if (!wcscmp(arg, L"--inspect")) {
            if (++i >= argc) { dn_error(&err, "Specify a RAW file to inspect"); goto done; }
            result = dn_raw_inspect(argv[i], &err) ? 0 : 1; goto done;
        }
        if (!wcscmp(arg, L"--cpu")) { job->options.cpu = 1; continue; }
        if (!wcscmp(arg, L"--offline")) { job->options.offline = 1; continue; }
        if (!wcscmp(arg, L"--no-tui")) { no_tui = 1; continue; }
        if (!wcscmp(arg, L"--models")) { dn_error(&err, "The model directory is fixed: models beside this executable. Remove --models and use 'denoise.exe model'"); goto done; }
        if (!wcsncmp(arg, L"--", 2)) {
            if (++i >= argc) { dn_error(&err, "Missing option value"); goto done; }
            value = argv[i];
            if (!wcscmp(arg, L"--output")) job->options.output = value;
            else if (!wcscmp(arg, L"--device")) {
                if (!integer(value, 0, 1024, &job->options.device)) { dn_error(&err, "Invalid GPU index"); goto done; }
            } else if (!wcscmp(arg, L"--tile")) {
                if (!integer(value, 128, 512, &job->options.tile) || job->options.tile % 64) { dn_error(&err, "Tile size must be 128..512 and divisible by 64"); goto done; }
            } else if (!wcscmp(arg, L"--model")) {
                if (!model_id(value, selected, sizeof(selected)) || !dn_model_select(0, 50, selected)) { dn_error(&err, "Unknown model ID or alias. Open 'denoise.exe model' to choose one"); goto done; }
            } else { dn_error(&err, "Unknown option"); goto done; }
        } else if (positional++ == 0) job->options.input = arg;
        else if (positional == 2) {
            if (!integer(arg, 0, 100, &job->options.amount)) { dn_error(&err, "Strength must be an integer from 0 through 100"); goto done; }
        } else { dn_error(&err, "Too many positional arguments"); goto done; }
    }
    if (!job->options.input || job->options.amount < 0) { help(); dn_error(&err, "Specify an image path and strength from 0 through 100"); goto done; }
    used_tui = !no_tui && dn_tui_available();
    if (used_tui) result = dn_tui_run("DENOISE IMAGE", job->options.input, dn_job_run, job, &err) ? 0 : 1;
    else {
        dn_cancel_reset(); dn_set_progress(dn_plain_progress, NULL);
        result = dn_job_run(job, &err) ? 0 : 1; dn_set_progress(NULL, NULL);
        if (!result) fwprintf(stdout, L"Saved: %ls\n", job->output);
        if (!result) fprintf(stderr, "Completed in %.2f s\n", job->elapsed);
    }
done:
    if (result && err.message[0] && !used_tui) fprintf(stderr, "Error: %s\n", err.message);
    free(interactive); free(job); return result;
}
