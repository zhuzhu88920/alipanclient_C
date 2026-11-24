#include "pan.h"
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define MAX_PARTS 3
#define DL_CHUNK  65536
#define PART2_MIN (512LL * 1024)
#define PART3_MIN (4LL * 1024 * 1024)

static char g_dl_err[512];

const char *download_error(void) { return g_dl_err[0] ? g_dl_err : "下载失败"; }

static void dlerr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_dl_err, sizeof g_dl_err, fmt, ap);
    va_end(ap);
}

char *safe_name(const char *s)
{
    Buf b;
    char *t, *start, *end;
    const char *p;
    if (buf_init(&b, 128)) return NULL;
    for (p = s ? s : ""; *p; p++) {
        char c = *p;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        buf_append(&b, &c, 1);
    }
    t = buf_take(&b);
    if (!t) return NULL;
    start = t;
    while (*start && is_space_ch(*start)) start++;
    end = t + strlen(t);
    while (end > start && is_space_ch(end[-1])) end--;
    *end = 0;
    if (start != t) memmove(t, start, (size_t)(end - start) + 1);
    return t;
}

static int ensure_dir(const char *dir)
{
    Buf p;
    const char *q;
    wchar_t *w = NULL;
    if (!dir || !*dir) return -1;
    if (buf_init(&p, 256)) return -1;
    for (q = dir; *q; q++) {
        buf_append(&p, q, 1);
        if (*q == '\\' || *q == '/') {
            w = NULL;
            if (utf8_to_utf16(p.data, &w) == 0) { CreateDirectoryW(w, NULL); free(w); }
        }
    }
    w = NULL;
    if (utf8_to_utf16(p.data, &w) == 0) { CreateDirectoryW(w, NULL); free(w); }
    buf_free(&p);
    return 0;
}

/* 返回 1 表示是普通文件并给出大小；目录或不存在返回 0 */
static int file_size(const char *path, int64_t *sz)
{
    wchar_t *w = NULL;
    WIN32_FILE_ATTRIBUTE_DATA d;
    BOOL ok;
    if (utf8_to_utf16(path, &w)) return 0;
    ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    *sz = (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    return 1;
}

static int delete_file_u8(const char *path)
{
    wchar_t *w = NULL;
    int ok;
    if (utf8_to_utf16(path, &w)) return 0;
    ok = DeleteFileW(w) ? 1 : 0;
    free(w);
    return ok;
}

static int rename_file_u8(const char *from, const char *to)
{
    wchar_t *wf = NULL, *wt = NULL;
    int ok;
    if (utf8_to_utf16(from, &wf) || utf8_to_utf16(to, &wt)) {
        free(wf); free(wt); return 0;
    }
    ok = MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
    free(wf); free(wt);
    return ok;
}

static char *host_of(const char *url)
{
    const char *p = strstr(url, "://"), *e;
    if (!p) return str_dup("-");
    p += 3;
    e = p;
    while (*e && *e != '/') e++;
    return str_dup_n(p, (size_t)(e - p));
}

void download_result_free(DownloadResult *r)
{
    if (!r) return;
    free(r->path);
    memset(r, 0, sizeof *r);
}

/* ---------- 分片续传位点（每个分片一个小文件） ---------- */

static void delete_idx_files(const char *target, int n)
{
    Buf b;
    int i;
    if (buf_init(&b, 256)) return;
    for (i = 0; i < n; i++) {
        b.len = 0; if (b.data) b.data[0] = 0;
        buf_printf(&b, "%s.p%d", target, i);
        delete_file_u8(b.data);
    }
    buf_free(&b);
}

static int read_idx(const char *path, int64_t *v)
{
    wchar_t *w = NULL;
    FILE *f;
    long long x = 0;
    if (utf8_to_utf16(path, &w)) return -1;
    f = _wfopen(w, L"r");
    free(w);
    if (!f) return -1;
    if (fscanf(f, "%lld", &x) != 1) { fclose(f); return -1; }
    fclose(f);
    if (x < 0) return -1;
    *v = (int64_t)x;
    return 0;
}

static void write_idx(const char *path, int64_t v)
{
    wchar_t *w = NULL;
    FILE *f;
    if (v < 0) v = 0;
    if (utf8_to_utf16(path, &w)) return;
    f = _wfopen(w, L"w");
    free(w);
    if (!f) return;
    fprintf(f, "%lld", (long long)v);
    fclose(f);
}

static int write_at(HANDLE h, const void *buf, DWORD n, int64_t offset)
{
    OVERLAPPED ov;
    DWORD wrote = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)((uint64_t)offset & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    if (!WriteFile(h, buf, n, &wrote, &ov)) return -1;
    return wrote == n ? 0 : -1;
}

static int set_file_len(HANDLE h, int64_t n)
{
    LARGE_INTEGER li;
    li.QuadPart = n;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return -1;
    return SetEndOfFile(h) ? 0 : -1;
}

typedef struct {
    const char *url;
    HANDLE file;
    int64_t start, end;          /* [start, end) */
    int64_t resume;              /* 本段已完成字节 */
    volatile LONG64 *shared_done;
    volatile LONG *abort;
    char *idx_path;
    int err;
} PartCtx;

static unsigned __stdcall part_worker(void *arg)
{
    PartCtx *w = (PartCtx *)arg;
    HttpStream *s = NULL;
    char range[80], buf[DL_CHUNK];
    int64_t pos, expect, clen = -1;
    int status = 0;
    size_t got;
    uint64_t last_idx;

    pos = w->start + w->resume;
    if (pos >= w->end) { write_idx(w->idx_path, w->resume); return 0; }

    snprintf(range, sizeof range, "bytes=%lld-%lld",
             (long long)pos, (long long)(w->end - 1));
    if (http_stream_open(w->url, range, &s, &status, &clen)) {
        w->err = 1; goto done;
    }
    if (status != 206) {                 /* 服务端不接受 Range：通知主线程回退 */
        w->err = 2;
        InterlockedExchange(w->abort, 1);
        goto done;
    }
    expect = w->end - pos;
    if (clen > 0 && clen != expect) {
        w->err = 3;
        InterlockedExchange(w->abort, 1);
        goto done;
    }

    last_idx = tick_ms();
    for (;;) {
        if (http_stream_read(s, buf, sizeof buf, &got)) { w->err = 4; break; }
        if (got > 0) {
            if ((int64_t)got > expect) { w->err = 5; break; }
            if (write_at(w->file, buf, (DWORD)got, pos)) { w->err = 6; break; }
            pos += (int64_t)got;
            expect -= (int64_t)got;
            InterlockedExchangeAdd64(w->shared_done, (LONG64)got);
            if (tick_ms() - last_idx >= 2000) {
                write_idx(w->idx_path, pos - w->start);  /* 防止进程被强杀丢断点 */
                last_idx = tick_ms();
            }
        }
        if (got == 0) break;
        if (g_interrupted || InterlockedCompareExchange(w->abort, 0, 0)) {
            w->err = 7; break;
        }
    }
    if (!w->err && expect != 0) w->err = 8;

done:
    write_idx(w->idx_path, pos - w->start);
    if (s) http_stream_close(s);
    return 0;
}

/* 返回 0=成功, 1=需要回退单连接, -1=失败（保留断点） */
static int try_parallel(const char *url, const char *target, const char *part,
                        int64_t want, int parts, const char *final_name,
                        DownloadResult *r, Buf *msg)
{
    PartCtx ctx[MAX_PARTS];
    HANDLE threads[MAX_PARTS];
    char *idx_paths[MAX_PARTS];
    int64_t seg[MAX_PARTS] = {0}, res[MAX_PARTS] = {0};
    int64_t pos = 0, sum = 0, shared_now = 0;
    volatile LONG abort = 0;
    volatile LONG64 shared = 0;
    HANDLE fh = NULL;
    wchar_t *wp = NULL;
    Buf ib;
    int have_idx = 0, i, nth = 0, ret = -1;
    Progress *rep = NULL;
    uint64_t last;

    memset(ctx, 0, sizeof ctx);
    memset(threads, 0, sizeof threads);
    memset(idx_paths, 0, sizeof idx_paths);
    if (buf_init(&ib, 256)) return -1;

    for (i = 0; i < parts; i++) {
        ib.len = 0; if (ib.data) ib.data[0] = 0;
        buf_printf(&ib, "%s.p%d", target, i);
        idx_paths[i] = str_dup(ib.data);
        if (!idx_paths[i]) goto done;
        if (read_idx(idx_paths[i], &res[i]) == 0) have_idx = 1;
    }
    for (i = 0; i < parts; i++) seg[i] = want / parts + (i < want % parts ? 1 : 0);
    if (!have_idx) {
        int64_t psz = 0;
        if (file_size(part, &psz) && psz > 0) {
            if (psz >= want) delete_file_u8(part);   /* 预分配满但无位点：有空洞，重来 */
            else res[0] = psz < seg[0] ? psz : seg[0]; /* 旧版单连接 .part：顺序续传 */
        }
    }
    for (i = 0; i < parts; i++) {
        if (res[i] < 0) res[i] = 0;
        if (res[i] > seg[i]) res[i] = seg[i];
        sum += res[i];
        ctx[i].start = pos;
        pos += seg[i];
        ctx[i].end = pos;
    }

    if (utf8_to_utf16(part, &wp) == 0)
        fh = CreateFileW(wp, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wp); wp = NULL;
    if (!fh || fh == INVALID_HANDLE_VALUE) { fh = NULL; dlerr("无法写入 .part 文件"); goto done; }
    if (set_file_len(fh, want)) { dlerr("无法预分配下载文件"); goto done; }

    for (i = 0; i < parts; i++) {
        ctx[i].url = url;
        ctx[i].file = fh;
        ctx[i].resume = res[i];
        ctx[i].shared_done = &shared;
        ctx[i].abort = &abort;
        ctx[i].idx_path = idx_paths[i];
        if (res[i] == seg[i]) continue;
        threads[nth] = (HANDLE)_beginthreadex(NULL, 0, part_worker, &ctx[i], 0, NULL);
        if (!threads[nth]) { dlerr("无法启动下载线程"); InterlockedExchange(&abort, 1); break; }
        nth++;
    }

    rep = progress_new();
    last = tick_ms();
    progress_update(rep, sum, want);
    for (;;) {
        int alive = 0;
        for (i = 0; i < nth; i++) {
            if (!threads[i]) continue;
            if (WaitForSingleObject(threads[i], 0) == WAIT_OBJECT_0) {
                CloseHandle(threads[i]); threads[i] = NULL;
            } else alive++;
        }
        if (!alive) break;
        if (g_interrupted) InterlockedExchange(&abort, 1);
        if (tick_ms() - last >= 100) {
            shared_now = (int64_t)InterlockedExchangeAdd64(&shared, 0);
            progress_update(rep, sum + shared_now, want);
            last = tick_ms();
        }
        Sleep(20);
    }
    shared_now = (int64_t)InterlockedExchangeAdd64(&shared, 0);
    progress_update(rep, sum + shared_now, want);
    progress_clear(rep); progress_free(rep); rep = NULL;

    if (InterlockedCompareExchange(&abort, 0, 0)) {
        dlerr("服务端不支持分片下载，已回退单连接");
        delete_idx_files(target, parts);
        CloseHandle(fh); fh = NULL;
        delete_file_u8(part);
        ret = 1; goto done;
    }
    for (i = 0; i < parts; i++) {
        if (ctx[i].err) {
            dlerr("分片 %d 失败(code %d)，已保留断点", i + 1, ctx[i].err);
            ret = -1; goto done;
        }
    }
    CloseHandle(fh); fh = NULL;
    delete_idx_files(target, parts);

    {
        int64_t existing = 0;
        if (file_size(target, &existing)) delete_file_u8(target);
        if (!rename_file_u8(part, target)) { dlerr("完成重命名失败"); goto done; }
    }
    r->bytes = want;
    buf_printf(msg, "下载完成 %s %lld 字节 -> %s（%d 分片）",
               final_name, (long long)want, target, parts);
    log_only("OK", msg->data);
    ret = 0;

done:
    if (rep) { progress_clear(rep); progress_free(rep); }
    for (i = 0; i < MAX_PARTS; i++) if (threads[i]) CloseHandle(threads[i]);
    if (fh && fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    for (i = 0; i < MAX_PARTS; i++) free(idx_paths[i]);
    buf_free(&ib);
    return ret;
}

/* ---------- 单连接（小文件 / Range 不支持时回退） ---------- */

static int download_single(const char *url, const char *target, const char *part,
                           int64_t want, const char *final_name,
                           DownloadResult *r, Buf *msg)
{
    char range[64], buf[DL_CHUNK];
    int64_t existing = 0, offset = 0, done, total, clen = -1, psz = 0;
    int status = 0, copy_err = 0;
    size_t got;
    uint64_t last_tick;
    FILE *f = NULL;
    wchar_t *wp = NULL;
    HttpStream *s = NULL;
    Progress *rep = NULL;

    delete_idx_files(target, MAX_PARTS);
    if (file_size(part, &psz)) {
        if (want <= 0 || psz < want) offset = psz;
        else { delete_file_u8(part); offset = 0; }
    }
    if (offset > 0) snprintf(range, sizeof range, "bytes=%lld-", (long long)offset);
    else range[0] = 0;
    if (http_stream_open(url, range[0] ? range : NULL, &s, &status, &clen)) {
        dlerr("%s", http_error()); goto done;
    }
    if (status != 200 && status != 206) {
        dlerr("CDN 返回 HTTP %d", status); goto done;
    }
    if (offset > 0 && status == 200) offset = 0;
    total = want;
    if (clen > 0) total = offset + clen;

    if (utf8_to_utf16(part, &wp) == 0)
        f = _wfopen(wp, offset > 0 ? L"ab" : L"wb");
    free(wp); wp = NULL;
    if (!f) { dlerr("无法写入 .part 文件"); goto done; }

    done = offset;
    last_tick = tick_ms();
    rep = progress_new();
    for (;;) {
        if (http_stream_read(s, buf, sizeof buf, &got)) { copy_err = 1; break; }
        if (got > 0) {
            if (fwrite(buf, 1, got, f) != got) { copy_err = 2; break; }
            done += (int64_t)got;
            if (tick_ms() - last_tick >= 100) {
                progress_update(rep, done, total);
                last_tick = tick_ms();
            }
        }
        if (got == 0) break;
        if (g_interrupted) { copy_err = 3; break; }
    }
    progress_update(rep, done, total);
    progress_clear(rep); progress_free(rep); rep = NULL;
    if (f) { fclose(f); f = NULL; }
    if (s) { http_stream_close(s); s = NULL; }

    if (copy_err) { dlerr("下载中断（已保留 .part 供续传）"); goto done; }
    if (g_interrupted) { dlerr("已中断（已保留 .part 供续传）"); goto done; }
    if (want > 0 && done != want) {
        dlerr("大小不符: %lld/%lld（已保留 .part 供续传）",
              (long long)done, (long long)want);
        goto done;
    }
    if (file_size(target, &existing)) delete_file_u8(target);
    if (!rename_file_u8(part, target)) { dlerr("完成重命名失败"); goto done; }
    r->bytes = done;
    buf_printf(msg, "下载完成 %s %lld 字节 -> %s",
               final_name, (long long)done, target);
    log_only("OK", msg->data);
    return 0;

done:
    if (rep) { progress_clear(rep); progress_free(rep); }
    if (f) fclose(f);
    if (s) http_stream_close(s);
    return -1;
}

int download_file(Client *c, const FileItem *it, const char *outdir, DownloadResult *r)
{
    Buf nb, target, part, msg;
    char *name = NULL, *final_name = NULL, *url = NULL, *host = NULL;
    int64_t want, existing = 0;
    int parts = 1, ret = -1, rc;

    memset(r, 0, sizeof *r);
    buf_init(&nb, 128); buf_init(&target, 256);
    buf_init(&part, 256); buf_init(&msg, 256);

    name = safe_name(it->file_name);
    if (!name) goto done;
    buf_append_cstr(&nb, name);
    if (it->type == 1) buf_append_cstr(&nb, ".zip");
    final_name = buf_take(&nb);
    if (!final_name) goto done;
    buf_printf(&target, "%s\\%s", outdir, final_name);
    buf_printf(&part, "%s.part", target.data);
    r->path = str_dup(target.data);
    want = it->size;

    if (want > 0 && file_size(target.data, &existing) && existing == want) {
        r->skipped = 1;
        r->bytes = want;
        ret = 0;
        goto done;
    }
    if (ensure_dir(outdir)) { dlerr("无法创建输出目录"); goto done; }
    if (api_download_url(c, it, &url)) {
        dlerr("%s", api_error());
        goto done;
    }
    if (c->verbose) {
        host = host_of(url);
        out("CDN: %s\n", host ? host : "-");
        free(host); host = NULL;
    }

    if (want >= PART3_MIN) parts = 3;
    else if (want >= PART2_MIN) parts = 2;

    if (parts > 1) {
        rc = try_parallel(url, target.data, part.data, want, parts,
                          final_name, r, &msg);
        if (rc == 0) { ret = 0; goto done; }
        if (rc < 0) goto done;
        /* rc == 1：服务端不接受 Range，回退单连接 */
    }
    if (download_single(url, target.data, part.data, want, final_name, r, &msg)) goto done;
    ret = 0;

done:
    free(name); free(final_name); free(url); free(host);
    buf_free(&nb); buf_free(&target); buf_free(&part); buf_free(&msg);
    if (ret != 0) { free(r->path); r->path = NULL; r->bytes = 0; }
    return ret;
}
