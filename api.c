#include "pan.h"
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define CLIENT_ID    "cf9f70e8fc61430f8ec5ab5cadf31375"
#define AUTH_KEY     "AE8627B0296A4126A1434999C45ECAB2"

static const char *token_host(void)
{
    const char *e = getenv("ALIPAN_TOKEN_HOST");
    return (e && *e) ? e : "https://api.tickstep.com";
}
static const char *openapi_host(void)
{
    const char *e = getenv("ALIPAN_API_BASE");
    return (e && *e) ? e : "https://openapi.alipan.com";
}

/* json_escape 只转义内容不加引号，这里补上两端引号 */
static void append_json_str(Buf *b, const char *s)
{
    buf_append_cstr(b, "\"");
    json_escape(b, s ? s : "");
    buf_append_cstr(b, "\"");
}

static char g_api_err[512];
const char *api_error(void) { return g_api_err[0] ? g_api_err : "接口调用失败"; }
static void api_seterr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_api_err, sizeof g_api_err, fmt, ap);
    va_end(ap);
}

int client_init(Client *c, const Config *cfg, int verbose)
{
    memset(c, 0, sizeof *c);
    c->cfg = cfg;
    c->verbose = verbose;
    if (cfg->access_token) c->token = str_dup(cfg->access_token);
    if (cfg->drive_id) c->drive_id = str_dup(cfg->drive_id);
    return 0;
}
void client_free(Client *c)
{
    if (!c) return;
    free(c->token); free(c->drive_id);
    memset(c, 0, sizeof *c);
}

int api_raw(Client *c, const char *method, const char *url,
            const char *body, HttpResponse *r)
{
    const char *names[4], *vals[4];
    Buf auth;
    size_t nh = 0;
    int rc;
    names[nh] = "content-type"; vals[nh++] = "application/json";
    if (buf_init(&auth, 512) == 0) {
        buf_printf(&auth, "Bearer %s", c->token ? c->token : "");
        names[nh] = "authorization"; vals[nh++] = auth.data;
    }
    rc = http_simple(method, url, names, vals, nh, body,
                     body ? strlen(body) : 0, 1, r);
    if (rc != 0) api_seterr("%s", http_error());
    buf_free(&auth);
    return rc;
}

/* ---------- 文件列表容器 ---------- */

void filelist_init(FileList *l) { memset(l, 0, sizeof *l); }
void fileitem_free(FileItem *it)
{
    if (!it) return;
    free(it->file_name); free(it->id);
    memset(it, 0, sizeof *it);
}
void filelist_free(FileList *l)
{
    size_t i;
    if (!l) return;
    for (i = 0; i < l->n; i++) fileitem_free(&l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}
int filelist_push(FileList *l, FileItem it)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 32;
        FileItem *p = (FileItem *)realloc(l->v, cap * sizeof(FileItem));
        if (!p) return -1;
        l->v = p; l->cap = cap;
    }
    l->v[l->n++] = it;
    return 0;
}

/* ---------- 授权（方案A：中转服务） ---------- */

static int token_get(const char *url, JDoc *d, int *data_out)
{
    HttpResponse r;
    int k, code, ret = -1;
    http_response_init(&r);
    if (http_simple("GET", url, NULL, NULL, 0, NULL, 0, 1, &r)) {
        api_seterr("%s", http_error()); goto done;
    }
    if (r.status != 200) { api_seterr("HTTP %d", r.status); goto done; }
    if (json_parse(d, r.body.data, r.body.len) < 0 || d->n == 0) {
        api_seterr("响应不是 JSON"); goto done;
    }
    k = json_obj_get(d, 0, "code");
    code = (k >= 0) ? (int)json_int(d, k, NULL) : 0;
    if (code != 0) {
        Buf msg;
        if (buf_init(&msg, 256) == 0) {
            int mk = json_obj_get(d, 0, "msg");
            if (mk >= 0) json_str(d, mk, &msg);
            api_seterr("服务返回错误 %d: %s", code, msg.data ? msg.data : "");
            buf_free(&msg);
        } else api_seterr("服务返回错误 %d", code);
        goto done;
    }
    *data_out = json_obj_get(d, 0, "data");
    if (*data_out < 0) { api_seterr("响应缺少 data"); goto done; }
    ret = 0;
done:
    http_response_free(&r);
    return ret;
}

static char *json_get_str(const JDoc *d, int obj, const char *key)
{
    int k = json_obj_get(d, obj, key);
    char *s = NULL;
    if (k >= 0) {
        Buf b;
        if (buf_init(&b, 128) == 0 && json_str(d, k, &b) == 0) s = buf_take(&b);
        else buf_free(&b);
    }
    return s;
}

static int open_browser(const char *url)
{
    wchar_t *w = NULL;
    HINSTANCE h;
    if (utf8_to_utf16(url, &w)) return -1;
    h = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    if ((intptr_t)h <= 32) { api_seterr("无法打开浏览器，请手动复制上面的链接"); return -1; }
    return 0;
}

static int fetch_drive_id(Client *c, Config *cfg)
{
    Buf url, body;
    HttpResponse r;
    JDoc d;
    char *id = NULL;
    int ret = -1, k;
    memset(&d, 0, sizeof d);
    if (buf_init(&url, 256) || buf_init(&body, 128)) {
        buf_free(&url); buf_free(&body); return -1;
    }
    buf_printf(&url, "%s/adrive/v1.0/user/getDriveInfo", openapi_host());
    buf_append_cstr(&body, "{}");
    http_response_init(&r);
    if (api_raw(c, "POST", url.data, body.data, &r)) goto done;
    if (r.status != 200) { api_seterr("获取网盘信息失败: HTTP %d", r.status); goto done; }
    if (json_parse(&d, r.body.data, r.body.len) < 0 || d.n == 0) {
        api_seterr("解析网盘信息失败"); goto done;
    }
    id = json_get_str(&d, 0, "default_drive_id");
    if (!id || !*id) { free(id); id = json_get_str(&d, 0, "resource_drive_id"); }
    if (!id || !*id) { api_seterr("未获取到 drive_id（请确认已授权 file 权限）"); goto done; }
    k = 0; (void)k;
    free(cfg->drive_id); cfg->drive_id = id; id = NULL;
    free(c->drive_id); c->drive_id = str_dup(cfg->drive_id);
    ret = 0;
done:
    free(id);
    json_free(&d);
    http_response_free(&r);
    buf_free(&url); buf_free(&body);
    return ret;
}

int api_login(Client *c, Config *cfg, const char *cfg_path)
{
    Buf u, auth_url;
    JDoc d;
    int data = -1, ret = -1;
    char *ticket = NULL, *token = NULL, *tu = NULL;
    int64_t exp = 0;
    memset(&d, 0, sizeof d);

    out("正在获取登录二维码...\n");
    if (buf_init(&u, 512) || buf_init(&auth_url, 1024)) {
        buf_free(&u); buf_free(&auth_url); api_seterr("内存不足"); return -1;
    }
    buf_printf(&u, "%s/auth/tickstep/aliyunpan/token/qrcode/create"
                   "?ip=127.0.0.1&os=windows&arch=amd64&version=%s&key=%s",
               token_host(), APP_VERSION, AUTH_KEY);
    if (token_get(u.data, &d, &data)) goto done;
    ticket = json_get_str(&d, data, "tokenId");
    if (!ticket || !*ticket) {
        tu = json_get_str(&d, data, "tokenUrl");
        if (tu) {
            char *p = strstr(tu, "tokenId=");
            if (p) {
                char *e;
                p += 8;
                e = p;
                while (*e && *e != '&' && *e != '#') e++;
                free(ticket);
                ticket = str_dup_n(p, (size_t)(e - p));
            }
        }
    }
    if (!ticket || !*ticket) { api_seterr("未获取到 ticketId"); goto done; }
    json_free(&d); memset(&d, 0, sizeof d); data = -1;

    buf_printf(&auth_url,
        "https://openapi.alipan.com/oauth/authorize"
        "?client_id=%s"
        "&redirect_uri=https%%3A%%2F%%2Fapi.tickstep.com%%2Fauth%%2Ftickstep%%2Faliyunpan%%2Ftoken%%2Fopenapi%%2F%s%%2Fauth2"
        "&scope=user:base,file:all:read,file:all:write,file:share:write,album:shared:read",
        CLIENT_ID, ticket);
    out("请在浏览器中完成授权（一次授权 + 一次扫码）：\n%s\n\n", auth_url.data);
    open_browser(auth_url.data);
    out("完成扫码后按 Enter 继续...\n");
    {
        char *line = NULL;
        if (!read_line(&line)) { api_seterr("无法读取输入"); goto done; }
        free(line);
    }

    u.len = 0; if (u.data) u.data[0] = 0;  /* buf_printf 是追加语义，必须清空复用 */
    buf_printf(&u, "%s/auth/tickstep/aliyunpan/token/common/%s/login", token_host(), ticket);
    if (token_get(u.data, &d, &data)) goto done;
    {
        int open = json_obj_get(&d, data, "openapi");
        int k;
        if (open < 0) { api_seterr("响应缺少 openapi token"); goto done; }
        token = json_get_str(&d, open, "accessToken");
        k = json_obj_get(&d, open, "expired");
        if (k >= 0) exp = json_int(&d, k, NULL);
    }
    if (!token || !*token) { api_seterr("未获取到 accessToken"); goto done; }

    free(cfg->access_token); cfg->access_token = token; token = NULL;
    free(cfg->ticket_id); cfg->ticket_id = str_dup(ticket);
    cfg->expired = exp;
    free(c->token); c->token = str_dup(cfg->access_token);

    if (fetch_drive_id(c, cfg)) goto done;
    if (config_save(cfg_path, cfg)) { api_seterr("保存配置失败: %s", cfg_path); goto done; }
    out("登录成功，凭据已保存到 %s\n", cfg_path);
    ret = 0;

done:
    free(ticket); free(token); free(tu);
    json_free(&d);
    buf_free(&u); buf_free(&auth_url);
    return ret;
}

/* ---------- 列目录 ---------- */

static int parse_file_item(const JDoc *d, int obj, FileItem *it)
{
    memset(it, 0, sizeof *it);
    it->file_name = json_get_str(d, obj, "name");
    it->id = json_get_str(d, obj, "file_id");
    {
        int k = json_obj_get(d, obj, "size");
        if (k >= 0) it->size = json_int(d, k, NULL);
    }
    {
        char *t = json_get_str(d, obj, "type");
        it->type = (t && strcmp(t, "folder") == 0) ? 1 : 0;
        free(t);
    }
    if (!it->file_name || !it->id) { fileitem_free(it); return -1; }
    return 0;
}

int api_list(Client *c, const char *parent, FileList *out)
{
    Buf url;
    char *marker = NULL;
    int ret = -1;
    if (!c->token || !*c->token) { api_seterr("未登录，请先运行 login"); return -1; }
    if (!c->drive_id || !*c->drive_id) { api_seterr("缺少 drive_id，请重新登录"); return -1; }
    if (buf_init(&url, 256)) return -1;
    buf_printf(&url, "%s/adrive/v1.0/openFile/list", openapi_host());
    for (;;) {
        Buf body;
        HttpResponse r;
        JDoc d;
        int items_arr, i;
        memset(&d, 0, sizeof d);
        if (buf_init(&body, 512)) goto done;
        buf_append_cstr(&body, "{\"drive_id\":");
        append_json_str(&body, c->drive_id);
        buf_append_cstr(&body, ",\"parent_file_id\":");
        append_json_str(&body, parent ? parent : "root");
        buf_append_cstr(&body,
            ",\"limit\":100,\"marker\":");
        append_json_str(&body, marker ? marker : "");
        buf_append_cstr(&body,
            ",\"order_by\":\"name\",\"order_direction\":\"ASC\",\"type\":\"all\",\"fields\":\"*\"}");
        http_response_init(&r);
        if (api_raw(c, "POST", url.data, body.data, &r)) {
            http_response_free(&r); buf_free(&body); goto done;
        }
        if (r.status != 200) {
            api_seterr("列目录失败: HTTP %d", r.status);
            http_response_free(&r); buf_free(&body); goto done;
        }
        if (json_parse(&d, r.body.data, r.body.len) < 0 || d.n == 0) {
            api_seterr("列目录响应不是 JSON");
            http_response_free(&r); buf_free(&body); goto done;
        }
        items_arr = json_obj_get(&d, 0, "items");
        if (items_arr >= 0 && d.t[items_arr].type == J_ARR) {
            for (i = 0; ; i++) {
                int obj = json_arr_at(&d, items_arr, (size_t)i);
                FileItem it;
                if (obj < 0) break;
                if (parse_file_item(&d, obj, &it) == 0) {
                    if (filelist_push(out, it)) { fileitem_free(&it); break; }
                }
            }
        }
        {
            char *nm = json_get_str(&d, 0, "next_marker");
            free(marker);
            marker = nm;
        }
        json_free(&d);
        http_response_free(&r);
        buf_free(&body);
        if (!marker || !*marker) break;
    }
    ret = 0;
done:
    free(marker);
    buf_free(&url);
    return ret;
}

int api_download_url(Client *c, const FileItem *it, char **url)
{
    Buf u, body;
    HttpResponse r;
    JDoc d;
    char *dl = NULL;
    int ret = -1;
    memset(&d, 0, sizeof d);
    *url = NULL;
    if (buf_init(&u, 256) || buf_init(&body, 256)) {
        buf_free(&u); buf_free(&body); return -1;
    }
    buf_append_cstr(&body, "{\"drive_id\":");
    append_json_str(&body, c->drive_id ? c->drive_id : "");
    buf_append_cstr(&body, ",\"file_id\":");
    append_json_str(&body, it->id ? it->id : "");
    buf_append_cstr(&body, ",\"expire_sec\":14400}");
    buf_printf(&u, "%s/adrive/v1.0/openFile/getDownloadUrl", openapi_host());
    http_response_init(&r);
    if (api_raw(c, "POST", u.data, body.data, &r)) goto done;
    if (r.status != 200) { api_seterr("获取下载链接失败: HTTP %d", r.status); goto done; }
    if (json_parse(&d, r.body.data, r.body.len) < 0 || d.n == 0) {
        api_seterr("解析下载链接失败"); goto done;
    }
    dl = json_get_str(&d, 0, "url");
    if (!dl || !*dl) { free(dl); dl = json_get_str(&d, 0, "cdn_url"); }
    if (!dl || !*dl) { api_seterr("响应中没有下载链接"); goto done; }
    *url = dl; dl = NULL;
    ret = 0;
done:
    free(dl);
    json_free(&d);
    http_response_free(&r);
    buf_free(&u); buf_free(&body);
    return ret;
}

/* 预留：分享参数接口，当前未实现 */
int api_share(Client *c, const char *ids_csv, char **share_url)
{
    (void)c; (void)ids_csv;
    *share_url = NULL;
    api_seterr("分享接口未实现（已预留参数）");
    return -1;
}
