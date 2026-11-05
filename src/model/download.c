#include "denoise.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int dn_file_verify(const wchar_t *path, const char *hash, size_t expected_bytes) {
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE state = NULL;
    unsigned char buffer[65536], digest[32];
    char hex[65];
    DWORD read;
    LARGE_INTEGER size;
    int ok = 0, i;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    if (!GetFileSizeEx(file, &size) || (uint64_t)size.QuadPart != expected_bytes) goto done;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(algorithm, &state, NULL, 0, NULL, 0, 0) < 0) goto done;
    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL)) goto done;
        if (!read) break;
        if (BCryptHashData(state, buffer, read, 0) < 0) goto done;
    }
    if (BCryptFinishHash(state, digest, sizeof(digest), 0) < 0) goto done;
    for (i = 0; i < 32; ++i) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
    ok = !strcmp(hex, hash);
done:
    if (state) BCryptDestroyHash(state);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static int seek_to(HANDLE file, uint64_t offset) {
    LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(file, pos, NULL, FILE_BEGIN);
}
/* Adopt a previous release's abandoned .part-PID-* file without touching it.
 * An actively written file has exclusive sharing and will not be opened here. */
static void adopt_partial(const wchar_t *path, HANDLE target, size_t maximum) {
    wchar_t *pattern = calloc(DN_PATH, sizeof(wchar_t));
    wchar_t *candidate = calloc(DN_PATH, sizeof(wchar_t));
    WIN32_FIND_DATAW item;
    HANDLE search = INVALID_HANDLE_VALUE, source = INVALID_HANDLE_VALUE;
    uint64_t largest = 0;
    if (!pattern || !candidate) goto done;
    if (swprintf_s(pattern, DN_PATH, L"%ls.part-*", path) < 0) goto done;
    search = FindFirstFileW(pattern, &item);
    if (search == INVALID_HANDLE_VALUE) goto done;
    do {
        uint64_t bytes = ((uint64_t)item.nFileSizeHigh << 32) | item.nFileSizeLow;
        wchar_t *slash;
        HANDLE next;
        if (item.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || bytes <= largest || bytes > maximum) continue;
        wcscpy_s(candidate, DN_PATH, path);
        slash = wcsrchr(candidate, L'\\');
        if (!slash) continue;
        slash[1] = 0;
        if (wcscat_s(candidate, DN_PATH, item.cFileName)) continue;
        next = CreateFileW(candidate, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (next == INVALID_HANDLE_VALUE) continue;
        if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
        source = next; largest = bytes;
    } while (FindNextFileW(search, &item));
    if (source != INVALID_HANDLE_VALUE) {
        unsigned char buffer[65536]; DWORD count, written;
        seek_to(target, 0);
        while (ReadFile(source, buffer, sizeof(buffer), &count, NULL) && count) {
            if (!WriteFile(target, buffer, count, &written, NULL) || written != count) {
                seek_to(target, 0); SetEndOfFile(target); break;
            }
        }
    }
done:
    if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
    if (search != INVALID_HANDLE_VALUE) FindClose(search);
    free(pattern); free(candidate);
}

/* One synchronous request on the worker thread. Cancellation is cooperative:
 * never close a synchronous WinHTTP request from another thread. */
static int receive_file(const wchar_t *url, HANDLE file, uint64_t start, size_t expected, const char *subject, DnError *err) {
    HINTERNET session = NULL, connection = NULL, request = NULL;
    URL_COMPONENTS parts = { sizeof(parts) };
    wchar_t host[512], resource[2048], range[128];
    DWORD status = 0, bytes = sizeof(status), n, written;
    unsigned char buffer[65536];
    uint64_t total = start;
    int ok = 0;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwHostNameLength >= 512 ||
        parts.dwUrlPathLength + parts.dwExtraInfoLength >= 2048) return dn_error(err, "Invalid HTTPS download URL");
    wcsncpy_s(host, 512, parts.lpszHostName, parts.dwHostNameLength);
    wcsncpy_s(resource, 2048, parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) wcsncat_s(resource, 2048, parts.lpszExtraInfo, parts.dwExtraInfoLength);
    dn_progress(DN_DOWNLOAD, start, expected, subject, start ? "Resuming download; connecting to server" : "Connecting to model server");
    session = WinHttpOpen(L"PhotoDenoise/1.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto network_error;
    if (!WinHttpSetTimeouts(session, 15000, 15000, 15000, 15000)) goto network_error;
    connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connection) goto network_error;
    request = WinHttpOpenRequest(connection, L"GET", resource, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) goto network_error;
    if (dn_cancelled()) goto canceled;
    if (start) swprintf_s(range, 128, L"Range: bytes=%llu-\r\n", start);
    if (!WinHttpSendRequest(request, start ? range : WINHTTP_NO_ADDITIONAL_HEADERS, start ? (DWORD)-1 : 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto network_error;
    if (dn_cancelled()) goto canceled;
    if (!WinHttpReceiveResponse(request, NULL) || !WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX)) goto network_error;
    if (status != 200 && status != 206) { dn_error(err, "Model server returned HTTP %lu", status); goto done; }
    if (status == 206) {
        wchar_t content_range[256]; DWORD length = sizeof(content_range);
        unsigned long long first, last, length_total;
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, content_range, &length, WINHTTP_NO_HEADER_INDEX) ||
            swscanf_s(content_range, L"bytes %llu-%llu/%llu", &first, &last, &length_total) != 3 ||
            first != start || last < first || last >= expected || length_total != expected) {
            dn_error(err, "Server returned an invalid download range"); goto done;
        }
    } else if (start) {
        total = 0;
        if (!seek_to(file, 0) || !SetEndOfFile(file)) goto disk_error;
    }
    if (!seek_to(file, total)) goto disk_error;
    for (;;) {
        if (dn_cancelled()) goto canceled;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &n)) goto network_error;
        if (!n) break;
        if (total + n > expected) { dn_error(err, "Download is larger than the pinned model file"); goto done; }
        if (!WriteFile(file, buffer, n, &written, NULL) || written != n) goto disk_error;
        total += n;
        dn_progress(DN_DOWNLOAD, total, expected, subject, "Downloading model file");
    }
    if (total != expected) { dn_error(err, "Download interrupted at %llu / %zu bytes; partial file retained", total, expected); goto done; }
    if (!FlushFileBuffers(file)) goto disk_error;
    ok = 1; goto done;
canceled:
    dn_error(err, "Canceled; partial download retained for resume"); goto done;
disk_error:
    dn_error(err, "Cannot write the models directory (Windows error %lu)", GetLastError()); goto done;
network_error:
    dn_error(err, "Network request failed (Windows error %lu); partial download retained", GetLastError());
done:
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

int dn_download(const wchar_t *url, const wchar_t *path, const char *hash, size_t expected_bytes, DnError *err) {
    wchar_t *temp = calloc(DN_PATH, sizeof(wchar_t));
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    char subject[192];
    const wchar_t *base = wcsrchr(path, L'\\');
    int attempt, ok = 0;
    if (!temp) return dn_error(err, "Out of memory");
    if (swprintf_s(temp, DN_PATH, L"%ls.download", path) < 0) { dn_error(err, "Download path is too long"); goto done; }
    if (!WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1, subject, sizeof(subject), NULL, NULL)) strcpy_s(subject, sizeof(subject), "Model file");
    for (attempt = 0; attempt < 3; ++attempt) {
        if (dn_cancelled()) { dn_error(err, "Canceled; partial download retained for resume"); break; }
        file = CreateFileW(temp, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) { dn_error(err, "Cannot open the model download. Another process may be installing it, or the executable folder is not writable (Windows error %lu).", GetLastError()); break; }
        if (!GetFileSizeEx(file, &size)) { dn_error(err, "Cannot read partial download size"); break; }
        if (!size.QuadPart) { adopt_partial(path, file, expected_bytes); if (!GetFileSizeEx(file, &size)) break; }
        if ((uint64_t)size.QuadPart > expected_bytes) {
            if (!seek_to(file, 0) || !SetEndOfFile(file)) { dn_error(err, "Cannot reset invalid partial download"); break; }
            size.QuadPart = 0;
        }
        if ((uint64_t)size.QuadPart == expected_bytes || receive_file(url, file, (uint64_t)size.QuadPart, expected_bytes, subject, err)) {
            CloseHandle(file); file = INVALID_HANDLE_VALUE;
            dn_progress(DN_VERIFY, 0, 0, subject, "Verifying downloaded file (SHA-256)");
            if (dn_file_verify(temp, hash, expected_bytes)) {
                if (dn_cancelled()) { dn_error(err, "Canceled; verified partial file retained"); break; }
                if (!MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { dn_error(err, "Cannot install model file (Windows error %lu)", GetLastError()); break; }
                ok = 1; break;
            }
            /* This is our own invalid partial, never an installed model. */
            if (!DeleteFileW(temp)) { dn_error(err, "Cannot remove invalid partial download"); break; }
            dn_error(err, "Model SHA-256 mismatch; retrying from the beginning");
        } else { FlushFileBuffers(file); CloseHandle(file); file = INVALID_HANDLE_VALUE; }
        if (!dn_cancelled() && attempt < 2) {
            dn_progress(DN_DOWNLOAD, 0, 0, subject, err->message);
            Sleep(400 * (attempt + 1));
        }
    }
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(temp); return ok;
}

