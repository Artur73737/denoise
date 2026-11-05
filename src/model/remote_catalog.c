#include "denoise.h"
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

int dn_http_catalog(const wchar_t *url, char **json, size_t *length, DnError *err) {
    HINTERNET session = NULL, connection = NULL, request = NULL;
    URL_COMPONENTS parts = {sizeof(parts)};
    wchar_t host[512], resource[2048];
    char *buffer = NULL;
    const size_t limit = 512 * 1024;
    DWORD status, bytes = sizeof(status), count;
    size_t used = 0;
    int ok = 0;
    *json = NULL; *length = 0;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url,0,0,&parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
        parts.dwHostNameLength >= 512 || parts.dwUrlPathLength + parts.dwExtraInfoLength >= 2048)
        return dn_error(err,"Invalid HTTPS catalog URL");
    wcsncpy_s(host,512,parts.lpszHostName,parts.dwHostNameLength);
    wcsncpy_s(resource,2048,parts.lpszUrlPath,parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) wcsncat_s(resource,2048,parts.lpszExtraInfo,parts.dwExtraInfoLength);
    buffer = malloc(limit + 1);
    if (!buffer) return dn_error(err,"Out of memory");
    dn_status(DN_DOWNLOAD,"Fetching online model catalog");
    session=WinHttpOpen(L"PhotoDenoise-Catalog/1.3",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if (!session) goto network_error;
    if (!WinHttpSetTimeouts(session,5000,5000,5000,5000)) goto network_error;
    connection=WinHttpConnect(session,host,parts.nPort,0);
    if (!connection) goto network_error;
    request=WinHttpOpenRequest(connection,L"GET",resource,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
    if (!request) goto network_error;
    if (dn_cancelled()) goto canceled;
    if (!WinHttpSendRequest(request,L"Accept: application/json\r\nCache-Control: no-cache\r\n",(DWORD)-1,WINHTTP_NO_REQUEST_DATA,0,0,0)) goto network_error;
    if (dn_cancelled()) goto canceled;
    if (!WinHttpReceiveResponse(request,NULL) || !WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,&status,&bytes,WINHTTP_NO_HEADER_INDEX)) goto network_error;
    if (status != 200) { dn_error(err,"Catalog server returned HTTP %lu",status); goto done; }
    for (;;) {
        char chunk[8192];
        if (dn_cancelled()) goto canceled;
        if (!WinHttpReadData(request,chunk,sizeof(chunk),&count)) goto network_error;
        if (!count) break;
        if (used + count > limit) { dn_error(err,"Catalog exceeds the 512 KiB limit"); goto done; }
        memcpy(buffer+used,chunk,count); used+=count;
    }
    if (!used || memchr(buffer,0,used)) { dn_error(err,"Catalog is empty or contains embedded NUL bytes"); goto done; }
    buffer[used]=0; *json=buffer; *length=used; buffer=NULL; ok=1; goto done;
canceled:
    dn_error(err,"Catalog refresh canceled"); goto done;
network_error:
    dn_error(err,"Cannot fetch catalog (Windows error %lu)",GetLastError());
done:
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    free(buffer); return ok;
}
