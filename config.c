#include "pan.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

char *exe_dir(void)
{
    wchar_t w[4096];
    DWORD n = GetModuleFileNameW(NULL, w, 4096);
    char *u = NULL, *slash;
    if (!n || n >= 4096) return str_dup(".");
    w[n] = 0;
    if (utf16_to_utf8(w, &u)) return str_dup(".");
    slash = strrchr(u, '\\');
    if (!slash) { free(u); return str_dup("."); }
    *slash = 0;
    return u;
}

static int read_file(const char *path, Buf *b)
{
    wchar_t *w = NULL;
    FILE *f;
    char tmp[4096];
    size_t n;
    if (utf8_to_utf16(path, &w)) return -1;
    f = _wfopen(w, L"rb");
    free(w);
    if (!f) return -1;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (buf_append(b, tmp, n)) { fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

static int write_file(const char *path, const char *data, size_t len)
{
    wchar_t *w = NULL;
    FILE *f;
    int rc = -1;
    if (utf8_to_utf16(path, &w)) return -1;
    f = _wfopen(w, L"wb");
    free(w);
    if (!f) return -1;
    if (fwrite(data, 1, len, f) == len) rc = 0;
    fclose(f);
    return rc;
}

void config_free(Config *c)
{
    if (!c) return;
    free(c->access_token); free(c->refresh_token);
    free(c->ticket_id); free(c->drive_id); free(c->download_path);
    memset(c, 0, sizeof *c);
}

static void take_str(JDoc *d, int root, const char *key, char **out)
{
    int k = json_obj_get(d, root, key);
    if (k >= 0) {
        Buf s;
        if (buf_init(&s, 128) == 0 && json_str(d, k, &s) == 0) {
            free(*out);
            *out = buf_take(&s);
        } else {
            buf_free(&s);
        }
    }
}

/* 返回 0=成功（token 可为空，由调用方决定是否登录），-2=配置文件解析失败 */
int config_load(const char *path, Config *c)
{
    Buf b;
    memset(c, 0, sizeof *c);
    if (buf_init(&b, 1024)) return -1;
    if (read_file(path, &b) == 0) {
        size_t off = 0;
        JDoc d;
        if (b.len >= 3 && (unsigned char)b.data[0] == 0xEF &&
            (unsigned char)b.data[1] == 0xBB && (unsigned char)b.data[2] == 0xBF) off = 3;
        memset(&d, 0, sizeof d);
        if (json_parse(&d, b.data + off, b.len - off) < 0 || d.n == 0) {
            json_free(&d); buf_free(&b); return -2;
        }
        take_str(&d, 0, "access_token", &c->access_token);
        take_str(&d, 0, "refresh_token", &c->refresh_token);
        take_str(&d, 0, "ticket_id", &c->ticket_id);
        take_str(&d, 0, "drive_id", &c->drive_id);
        take_str(&d, 0, "download_path", &c->download_path);
        {
            int k = json_obj_get(&d, 0, "expired");
            if (k >= 0) c->expired = json_int(&d, k, NULL);
        }
        json_free(&d);
    }
    buf_free(&b);
    return 0;
}

static void json_quote(Buf *b, const char *s)
{
    buf_append_cstr(b, "\"");
    if (s) json_escape(b, s);
    buf_append_cstr(b, "\"");
}

int config_save(const char *path, const Config *c)
{
    Buf b;
    int64_t exp = c->expired ? c->expired : (now_unix() + 30 * 86400);
    if (buf_init(&b, 1024)) return -1;
    buf_append_cstr(&b, "{\n  \"access_token\": ");
    json_quote(&b, c->access_token);
    buf_append_cstr(&b, ",\n  \"refresh_token\": ");
    json_quote(&b, c->refresh_token);
    buf_append_cstr(&b, ",\n  \"ticket_id\": ");
    json_quote(&b, c->ticket_id);
    buf_append_cstr(&b, ",\n  \"drive_id\": ");
    json_quote(&b, c->drive_id);
    buf_append_cstr(&b, ",\n  \"download_path\": ");
    json_quote(&b, c->download_path);
    buf_printf(&b, ",\n  \"expired\": %lld\n}\n", (long long)exp);
    {
        int rc = write_file(path, b.data, b.len);
        buf_free(&b);
        return rc;
    }
}

static int writable(const char *dir)
{
    Buf p;
    wchar_t *wd = NULL, *wp = NULL;
    FILE *f;
    int ok = 0;
    if (!dir || !*dir) return 0;
    if (utf8_to_utf16(dir, &wd)) return 0;
    CreateDirectoryW(wd, NULL);
    free(wd);
    if (buf_init(&p, 256)) return 0;
    if (buf_printf(&p, "%s\\~alipan.tmp", dir) == 0 &&
        utf8_to_utf16(p.data, &wp) == 0) {
        f = _wfopen(wp, L"wb");
        if (f) {
            fwrite("ok", 1, 2, f);
            fclose(f);
            DeleteFileW(wp);
            ok = 1;
        }
        free(wp);
    }
    buf_free(&p);
    return ok;
}

char *default_download_dir(const char *configured)
{
    char drives[26];
    size_t nd = 0, i;
    char cands[6][1024];
    size_t nc = 0;
    int n;
    const char *home;
    if (configured && *configured && writable(configured)) return str_dup(configured);
    for (i = 0; i < 26; i++) {
        wchar_t root[4];
        root[0] = (wchar_t)('A' + i); root[1] = L':'; root[2] = L'\\'; root[3] = 0;
        if (GetDriveTypeW(root) == DRIVE_FIXED) drives[nd++] = (char)('A' + i);
    }
    if (nd >= 2) {
        n = snprintf(cands[nc], sizeof cands[0], "%c:\\downloads", drives[1]);
        if (n > 0 && (size_t)n < sizeof cands[0]) nc++;
    }
    if (nd >= 1) {
        n = snprintf(cands[nc], sizeof cands[0], "%c:\\downloads", drives[0]);
        if (n > 0 && (size_t)n < sizeof cands[0]) nc++;
    }
    home = getenv("USERPROFILE");
    if (home) {
        n = snprintf(cands[nc], sizeof cands[0], "%s\\downloads", home);
        if (n > 0 && (size_t)n < sizeof cands[0]) nc++;
    }
    n = snprintf(cands[nc], sizeof cands[0], "downloads");
    if (n > 0 && (size_t)n < sizeof cands[0]) nc++;
    for (i = 0; i < nc; i++) if (writable(cands[i])) return str_dup(cands[i]);
    return str_dup(".");
}
