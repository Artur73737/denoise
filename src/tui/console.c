#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { MUTED = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED,
    TEXT = MUTED | FOREGROUND_INTENSITY, ACCENT = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    GOOD = FOREGROUND_GREEN | FOREGROUND_INTENSITY, BAD = FOREGROUND_RED | FOREGROUND_INTENSITY,
    SELECTED = BACKGROUND_BLUE | BACKGROUND_GREEN | BACKGROUND_INTENSITY };
typedef struct {
    HANDLE input, original, screen;
    DWORD input_mode;
    wchar_t title[512];
    CHAR_INFO *cells;
    int width, height;
    SMALL_RECT region;
} Ui;
typedef struct {
    CRITICAL_SECTION lock;
    DnProgress progress;
    DnError error;
    DnWork work;
    void *context;
    volatile LONG finished;
    int ok;
} WorkState;

int dn_tui_available(void) {
    DWORD a, b;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &a) && GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &b);
}
static int open_ui(Ui *ui, const wchar_t *title, DnError *err) {
    CONSOLE_CURSOR_INFO cursor = {1, FALSE};
    memset(ui, 0, sizeof(*ui));
    ui->input = GetStdHandle(STD_INPUT_HANDLE);
    if (!dn_tui_available() || !GetConsoleMode(ui->input, &ui->input_mode))
        return dn_error(err, "An interactive Windows terminal is required. Use 'model list' or 'model install <id>' in scripts");
    /* CONOUT$ refers to the currently active screen, including a parent TUI.
     * STD_OUTPUT_HANDLE still points at the original shell in nested views. */
    ui->original = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (ui->original == INVALID_HANDLE_VALUE) return dn_error(err, "Cannot access the active console screen");
    GetConsoleTitleW(ui->title, 512);
    ui->screen = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    if (ui->screen == INVALID_HANDLE_VALUE) { CloseHandle(ui->original); return dn_error(err, "Cannot create the terminal interface (Windows error %lu)", GetLastError()); }
    if (!SetConsoleMode(ui->input, (ui->input_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
        ENABLE_QUICK_EDIT_MODE | ENABLE_VIRTUAL_TERMINAL_INPUT)) | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT) ||
        !SetConsoleActiveScreenBuffer(ui->screen)) {
        SetConsoleMode(ui->input, ui->input_mode); CloseHandle(ui->screen); CloseHandle(ui->original); ui->screen = NULL;
        return dn_error(err, "Cannot activate the terminal interface (Windows error %lu)", GetLastError());
    }
    SetConsoleCursorInfo(ui->screen, &cursor);
    SetConsoleTitleW(title);
    return 1;
}
static void close_ui(Ui *ui) {
    SetConsoleMode(ui->input, ui->input_mode);
    SetConsoleActiveScreenBuffer(ui->original);
    SetConsoleTitleW(ui->title);
    if (ui->screen && ui->screen != INVALID_HANDLE_VALUE) CloseHandle(ui->screen);
    CloseHandle(ui->original);
    free(ui->cells);
}
static int frame(Ui *ui) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    CHAR_INFO *next;
    int width, height, i;
    if (!GetConsoleScreenBufferInfo(ui->screen, &info)) return 0;
    width = info.srWindow.Right - info.srWindow.Left + 1;
    height = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (width < 1 || height < 1 || width > 1000 || height > 500) return 0;
    if (width != ui->width || height != ui->height || !ui->cells) {
        next = realloc(ui->cells, (size_t)width * height * sizeof(*next));
        if (!next) return 0;
        ui->cells = next; ui->width = width; ui->height = height;
    }
    ui->region = info.srWindow;
    for (i = 0; i < width * height; ++i) { ui->cells[i].Char.UnicodeChar = L' '; ui->cells[i].Attributes = TEXT; }
    return 1;
}
static void wide(Ui *ui, int x, int y, int limit, WORD color, const wchar_t *value) {
    int i, width;
    if (y < 0 || y >= ui->height || x < 0 || x >= ui->width || limit < 1) return;
    width = limit < ui->width - x ? limit : ui->width - x;
    for (i = 0; i < width && value[i]; ++i) {
        ui->cells[y * ui->width + x + i].Char.UnicodeChar = value[i];
        ui->cells[y * ui->width + x + i].Attributes = color;
    }
    if (i == width && value[i]) ui->cells[y * ui->width + x + width - 1].Char.UnicodeChar = L'\x2026';
}
static void line(Ui *ui, int x, int y, int limit, WORD color, const char *value) {
    wchar_t converted[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, value, -1, converted, 2048)) wcscpy_s(converted, 2048, L"...");
    wide(ui, x, y, limit, color, converted);
}
static void format(Ui *ui, int x, int y, int limit, WORD color, const char *fmt, ...) {
    char value[2048]; va_list args;
    va_start(args, fmt); vsnprintf(value, sizeof(value), fmt, args); va_end(args);
    line(ui, x, y, limit, color, value);
}
static void wrap(Ui *ui, int x, int y, int width, int rows, WORD color, const char *value) {
    wchar_t converted[2048], piece[2048];
    const wchar_t *p = converted;
    int r;
    if (width < 1 || !MultiByteToWideChar(CP_UTF8, 0, value, -1, converted, 2048)) return;
    for (r = 0; r < rows && *p; ++r) {
        int n = 0, last_space = -1;
        while (p[n] && p[n] != L'\n' && n < width && n < 2047) { if (p[n] == L' ') last_space = n; ++n; }
        if (p[n] && p[n] != L'\n' && last_space > 0) n = last_space;
        wcsncpy_s(piece, 2048, p, n);
        wide(ui, x, y + r, width, color, piece);
        p += n; while (*p == L' ' || *p == L'\n') ++p;
    }
}
static void divider(Ui *ui, int y) {
    int x;
    if (y < 0 || y >= ui->height) return;
    for (x = 2; x < ui->width - 2; ++x) { ui->cells[y * ui->width + x].Char.UnicodeChar = L'\x2500'; ui->cells[y * ui->width + x].Attributes = MUTED; }
}
static void paint(Ui *ui) {
    COORD size = {(SHORT)ui->width, (SHORT)ui->height}, zero = {0,0};
    WriteConsoleOutputW(ui->screen, ui->cells, size, zero, &ui->region);
}
static int small(Ui *ui) {
    if (ui->width >= 72 && ui->height >= 24) return 0;
    line(ui, 1, 1, ui->width - 2, ACCENT, "PHOTO DENOISE");
    line(ui, 1, 3, ui->width - 2, TEXT, "Resize the terminal to at least 72 columns x 24 rows.");
    line(ui, 1, 5, ui->width - 2, MUTED, "Esc: back / cancel");
    return 1;
}
static int key(Ui *ui, KEY_EVENT_RECORD *out) {
    INPUT_RECORD input;
    DWORD count;
    if (WaitForSingleObject(ui->input, 50) != WAIT_OBJECT_0) return 0;
    if (!ReadConsoleInputW(ui->input, &input, 1, &count) || !count) return 0;
    if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) return 0;
    *out = input.Event.KeyEvent; return 1;
}
static int escape(const KEY_EVENT_RECORD *event) {
    return event->wVirtualKeyCode == VK_ESCAPE || ((event->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) && event->wVirtualKeyCode == 'C');
}
static void publish(void *context, const DnProgress *progress) {
    WorkState *state = context;
    EnterCriticalSection(&state->lock); state->progress = *progress; LeaveCriticalSection(&state->lock);
}
static DWORD WINAPI worker(void *context) {
    WorkState *state = context;
    state->ok = state->work(state->context, &state->error);
    InterlockedExchange(&state->finished, 1);
    return 0;
}
static int tui_run_ex(const char *title, const wchar_t *subtitle, DnWork work, void *context, DnError *err, int auto_return) {
    Ui ui;
    WorkState state = {0};
    HANDLE thread;
    ULONGLONG began = GetTickCount64(), sample_tick = began;
    uint64_t sample_bytes = 0;
    char sample_subject[192] = "";
    double speed = 0;
    int finished = 0, leave = 0;
    if (!open_ui(&ui, L"Photo Denoise", err)) return 0;
    InitializeCriticalSection(&state.lock); state.work = work; state.context = context;
    dn_cancel_reset(); dn_set_progress(publish, &state);
    thread = CreateThread(NULL, 0, worker, &state, 0, NULL);
    if (!thread) {
        dn_set_progress(NULL, NULL); DeleteCriticalSection(&state.lock); close_ui(&ui);
        return dn_error(err, "Cannot start the worker thread");
    }
    while (!leave) {
        DnProgress p;
        KEY_EVENT_RECORD event;
        ULONGLONG now = GetTickCount64();
        finished = InterlockedCompareExchange(&state.finished, 0, 0) != 0;
        if (finished && auto_return) break;
        EnterCriticalSection(&state.lock); p = state.progress; LeaveCriticalSection(&state.lock);
        if (p.stage == DN_DOWNLOAD) {
            if (strcmp(sample_subject, p.subject) || p.done < sample_bytes) {
                strcpy_s(sample_subject, sizeof(sample_subject), p.subject); sample_bytes = p.done; sample_tick = now; speed = 0;
            } else if (now - sample_tick >= 500) {
                speed = (p.done - sample_bytes) * 1000.0 / (double)(now - sample_tick);
                sample_bytes = p.done; sample_tick = now;
            }
        }
        if (frame(&ui)) {
            if (!small(&ui)) {
                int width = ui.width - 6, y = 10, x;
                char spinner[] = "|/-\\";
                line(&ui, 3, 1, width, ACCENT, "PHOTO DENOISE");
                line(&ui, 3, 3, width, TEXT, title);
                wide(&ui, 3, 5, width, MUTED, subtitle);
                divider(&ui, 7);
                format(&ui, 3, 9, width, finished ? (state.ok ? GOOD : BAD) : ACCENT,
                    "%c  %s", finished ? (state.ok ? '+' : '!') : spinner[(now / 150) % 4],
                    finished ? (state.ok ? "Completed" : dn_cancelled() ? "Canceled" : "Failed") : dn_cancelled() ? "Canceling after the current operation..." : dn_stage_name(p.stage));
                if (p.subject[0]) line(&ui, 3, ++y, width, TEXT, p.subject);
                wrap(&ui, 3, ++y, width, 3, finished && !state.ok ? BAD : TEXT, finished && !state.ok ? state.error.message : p.message);
                y += 4;
                if (p.total && !finished) {
                    double fraction = (double)p.done / (double)p.total;
                    int filled = (int)(fraction * (width - 9));
                    if (filled < 0) filled = 0; if (filled > width - 9) filled = width - 9;
                    for (x = 0; x < width - 9; ++x) {
                        wchar_t cell[] = {x < filled ? L'\x2588' : L'\x2591', 0};
                        wide(&ui, 3 + x, y, 1, x < filled ? ACCENT : MUTED, cell);
                    }
                    format(&ui, ui.width - 10, y, 7, TEXT, "%5.1f%%", fraction * 100.0);
                    if (p.stage == DN_DOWNLOAD) {
                        format(&ui, 3, y + 2, width, MUTED, "%.2f / %.2f MiB  |  %.2f MiB/s", p.done / 1048576.0, p.total / 1048576.0, speed / 1048576.0);
                        if (speed > 1024 && p.total > p.done) format(&ui, 3, y + 3, width, MUTED, "Estimated time remaining: %.0f s", (p.total - p.done) / speed);
                    } else format(&ui, 3, y + 2, width, MUTED, "%llu / %llu", p.done, p.total);
                }
                format(&ui, 3, ui.height - 4, width, MUTED, "Elapsed: %.1f s", (now - began) / 1000.0);
                divider(&ui, ui.height - 3);
                line(&ui, 3, ui.height - 2, width, TEXT, finished ? "Enter / Esc  Back" : "Esc / Ctrl+C  Cancel  |  Partial model downloads can be resumed");
            }
            paint(&ui);
        }
        if (key(&ui, &event)) {
            if (finished && (escape(&event) || event.wVirtualKeyCode == VK_RETURN)) leave = 1;
            else if (escape(&event)) dn_cancel();
        }
    }
    WaitForSingleObject(thread, INFINITE); CloseHandle(thread);
    dn_set_progress(NULL, NULL);
    if (!state.ok) *err = state.error;
    DeleteCriticalSection(&state.lock); close_ui(&ui);
    return state.ok;
}
int dn_tui_run(const char *title, const wchar_t *subtitle, DnWork work, void *context, DnError *err) {
    return tui_run_ex(title,subtitle,work,context,err,0);
}
int dn_tui_job(DnJob *job, DnError *err) {
    return tui_run_ex("DENOISE IMAGE",job->options.input,dn_job_run,job,err,1);
}
int dn_tui_catalog_refresh(DnError *err) {
    return tui_run_ex("FETCH MODEL CATALOG",L"Fetching current model profiles; cached/bundled fallback is available",dn_catalog_refresh,NULL,err,1);
}

int dn_tui_models(DnError *err) {
    Ui ui;
    DnInstall *install = calloc(1, sizeof(*install));
    int installed[128] = {0}, selected = 0, done = 0, top = 0;
    size_t i, count = dn_model_count();
    if (!install) return dn_error(err, "Out of memory");
    if (!count || count > 128 || !dn_models_directory(install->cache, err) || !open_ui(&ui, L"Photo Denoise - Model Library", err)) { free(install); return 0; }
    {
        DnError refresh_error = {{0}};
        tui_run_ex("FETCH MODEL CATALOG",L"GitHub online catalog; cached/bundled fallback when offline",dn_catalog_refresh,NULL,&refresh_error,1);
        dn_cancel_reset(); count=dn_model_count();
    }
    if (frame(&ui)) { line(&ui, 3, 2, ui.width - 6, ACCENT, "PHOTO DENOISE  /  MODEL LIBRARY"); line(&ui, 3, 4, ui.width - 6, TEXT, "Checking installed models and SHA-256 integrity..."); paint(&ui); }
    for (i = 0; i < count; ++i) installed[i] = dn_model_ready(dn_model_at(i), install->cache);
    while (!done) {
        KEY_EVENT_RECORD event;
        const DnModel *model = dn_model_at(selected);
        if (frame(&ui)) {
            if (!small(&ui)) {
                int width = ui.width - 6, visible = ui.height - 17, detail;
                if (visible > (int)count) visible=(int)count;
                if (selected < top) top=selected;
                if (selected >= top+visible) top=selected-visible+1;
                if (top+visible>(int)count) top=(int)count-visible;
                if (top<0) top=0;
                detail=6+visible;
                line(&ui, 3, 1, width, ACCENT, "PHOTO DENOISE  /  MODEL LIBRARY");
                line(&ui, 3, 2, width, MUTED, dn_catalog_status());
                format(&ui, 3, 4, width - 28, MUTED, "MODEL ID  (%d-%d of %zu)",top+1,top+visible,count);
                line(&ui, ui.width - 28, 4, 12, MUTED, "DISK SIZE");
                line(&ui, ui.width - 15, 4, 12, MUTED, "STATUS");
                for (i = (size_t)top; i < (size_t)(top+visible); ++i) {
                    const DnModel *item = dn_model_at(i);
                    int row = 5 + (int)i-top, x;
                    WORD color = (int)i == selected ? SELECTED : TEXT;
                    if ((int)i == selected) for (x = 2; x < ui.width - 2; ++x) ui.cells[row * ui.width + x].Attributes = SELECTED;
                    format(&ui, 3, row, width - 28, color, "%c %s", (int)i == selected ? '>' : ' ', item->id);
                    format(&ui, ui.width - 28, row, 12, color, "%.2f MiB", dn_model_size(item) / 1048576.0);
                    line(&ui, ui.width - 15, row, 12, (int)i == selected ? SELECTED : installed[i] ? GOOD : MUTED, installed[i] ? "Installed" : "Not ready");
                }
                divider(&ui, detail - 1);
                format(&ui, 3, detail, width, ACCENT, "%s  [%s]", model->title, model->channels == 1 ? "Grayscale" : "RGB");
                wrap(&ui, 3, detail + 1, width, 2, TEXT, model->description);
                wrap(&ui, 3, detail + 3, width, 2, MUTED, model->performance);
                format(&ui, 3, detail + 5, width, TEXT, "Use: --model %s", model->id);
                wide(&ui, 3, ui.height - 4, width, MUTED, install->cache);
                line(&ui, 3, ui.height - 3, width, MUTED, "Not ready = missing, partial or failed integrity check. Performance varies by device.");
                line(&ui, 3, ui.height - 2, width, ACCENT, "Up/Down Select   Enter Install/repair   R Fetch catalog   Esc/Q Back");
            }
            paint(&ui);
        }
        if (!key(&ui, &event)) continue;
        if (escape(&event) || event.wVirtualKeyCode == 'Q') done = 1;
        else if (event.wVirtualKeyCode == VK_UP || event.wVirtualKeyCode == 'K') selected = (selected + (int)count - 1) % (int)count;
        else if (event.wVirtualKeyCode == VK_DOWN || event.wVirtualKeyCode == 'J') selected = (selected + 1) % (int)count;
        else if (event.wVirtualKeyCode == VK_HOME) selected = 0;
        else if (event.wVirtualKeyCode == VK_END) selected = (int)count - 1;
        else if (event.wVirtualKeyCode == VK_NEXT) { selected+=7;if(selected>=(int)count)selected=(int)count-1; }
        else if (event.wVirtualKeyCode == VK_PRIOR) { selected-=7;if(selected<0)selected=0; }
        else if (event.wVirtualKeyCode == 'R') {
            DnError refresh_error={{0}};
            tui_run_ex("REFRESH MODEL CATALOG",L"Fetching latest curated model descriptions and pinned files",dn_catalog_refresh,NULL,&refresh_error,1);
            dn_cancel_reset(); count=dn_model_count();if(selected>=(int)count)selected=(int)count-1;
            for (i=0;i<count;++i) installed[i]=dn_model_ready(dn_model_at(i),install->cache);
        }
        else if (event.wVirtualKeyCode == VK_RETURN) {
            DnError install_error = {{0}};
            wchar_t name[128];
            install->model = model;
            MultiByteToWideChar(CP_UTF8, 0, model->id, -1, name, 128);
            dn_tui_run("INSTALL MODEL", name, dn_install_run, install, &install_error);
            installed[selected] = dn_model_ready(model, install->cache);
            dn_cancel_reset();
        }
    }
    close_ui(&ui); free(install); return 1;
}

int dn_tui_form(wchar_t *path, size_t capacity, int *strength, DnError *err) {
    Ui ui;
    wchar_t amount[4];
    size_t caret = wcslen(path);
    int focus = 0, done = 0, accepted = 0;
    const char *validation = "";
    swprintf_s(amount, 4, L"%d", *strength);
    if (!open_ui(&ui, L"Photo Denoise - Open Image", err)) return 0;
    while (!done) {
        KEY_EVENT_RECORD event;
        if (frame(&ui)) {
            if (!small(&ui)) {
                int width = ui.width - 6;
                size_t n = wcslen(path), start = n >= (size_t)width ? n - width + 1 : 0;
                line(&ui, 3, 1, width, ACCENT, "PHOTO DENOISE");
                line(&ui, 3, 3, width, TEXT, "DENOISE AN IMAGE");
                line(&ui, 3, 5, width, MUTED, "Source stays unchanged. Developed RAW/codec images become 16-bit TIFF.");
                line(&ui, 3, 8, width, focus == 0 ? ACCENT : MUTED, "IMAGE PATH");
                wide(&ui, 3, 9, width, TEXT, path + start);
                divider(&ui, 10);
                line(&ui, 3, 12, width, focus == 1 ? ACCENT : MUTED, "STRENGTH  0-100");
                wide(&ui, 3, 13, width, TEXT, amount);
                divider(&ui, 14);
                {
                    int cursor_x = focus ? 3 + (int)caret : 3 + (int)(caret >= start ? caret - start : 0);
                    int cursor_y = focus ? 13 : 9;
                    if (cursor_x >= 3 && cursor_x < ui.width - 3) ui.cells[cursor_y * ui.width + cursor_x].Attributes = SELECTED;
                }
                line(&ui, 3, 16, width, BAD, validation);
                line(&ui, 3, ui.height - 4, width, MUTED, "Manage downloads separately: denoise.exe model");
                line(&ui, 3, ui.height - 2, width, ACCENT, "Tab  Switch field   Enter  Continue/start   Esc  Quit");
            }
            paint(&ui);
        }
        if (!key(&ui, &event)) continue;
        if (escape(&event)) done = 1;
        else if (event.wVirtualKeyCode == VK_TAB) { focus = !focus; caret = wcslen(focus ? amount : path); validation = ""; }
        else if (event.wVirtualKeyCode == VK_RETURN) {
            wchar_t *end; long value = wcstol(amount, &end, 10);
            if (!path[0]) { validation = "Enter an image path"; focus = 0; caret = 0; }
            else if (focus == 0) { focus = 1; caret = wcslen(amount); }
            else if (!amount[0] || *end || value < 0 || value > 100) validation = "Strength must be an integer from 0 through 100";
            else { *strength = (int)value; done = accepted = 1; }
        } else {
            wchar_t *field = focus ? amount : path;
            size_t length = wcslen(field), limit = focus ? 4 : capacity;
            if (caret > length) caret = length;
            if (event.wVirtualKeyCode == VK_LEFT && caret) --caret;
            else if (event.wVirtualKeyCode == VK_RIGHT && caret < length) ++caret;
            else if (event.wVirtualKeyCode == VK_HOME) caret = 0;
            else if (event.wVirtualKeyCode == VK_END) caret = length;
            else if (event.wVirtualKeyCode == VK_BACK && caret) { memmove(field + caret - 1, field + caret, (length - caret + 1) * sizeof(wchar_t)); --caret; }
            else if (event.wVirtualKeyCode == VK_DELETE && caret < length) memmove(field + caret, field + caret + 1, (length - caret) * sizeof(wchar_t));
            else if (event.uChar.UnicodeChar >= 32 && length + 1 < limit && (!focus || (event.uChar.UnicodeChar >= '0' && event.uChar.UnicodeChar <= '9'))) {
                memmove(field + caret + 1, field + caret, (length - caret + 1) * sizeof(wchar_t)); field[caret++] = event.uChar.UnicodeChar;
            }
        }
    }
    if (accepted && path[0] == L'"' && wcslen(path) > 1 && path[wcslen(path)-1] == L'"') {
        size_t n = wcslen(path); memmove(path, path + 1, (n - 2) * sizeof(wchar_t)); path[n - 2] = 0;
    }
    close_ui(&ui); return accepted;
}
