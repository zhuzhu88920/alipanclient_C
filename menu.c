#include "pan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Client *c;
    Options *o;
    Config *cfg;
    const char *cfg_path;
    const char *outdir;
    char *parent;
    char *display;
    char **stack;
    size_t nstack, capstack;
    FileList items;
} Menu;

static char *trim(char *s)
{
    char *e;
    while (*s && is_space_ch(*s)) s++;
    e = s + strlen(s);
    while (e > s && is_space_ch(e[-1])) e--;
    *e = 0;
    return s;
}

static void print_menu_help(void)
{
    out("命令:\n"
        "  ls / re            显示 / 刷新当前目录\n"
        "  <数字>             进入文件夹，或查看文件后输入 1 下载\n"
        "  download <n|路径>  下载（路径相对根目录）\n"
        "  cd <n|..|/|路径>   切换目录\n"
        "  link <n>           打印该文件的临时下载链接（注意保密）\n"
        "  login              重新扫码登录\n"
        "  help / exit        帮助 / 退出\n"
        "说明: 分享接口已预留参数，当前版本未实现。\n");
}

static int confirm(const char *prompt)
{
    char *line = NULL, *t;
    int ok;
    out("%s (输入 1 继续): ", prompt);
    if (!read_line(&line)) return 0;
    t = trim(line);
    ok = strcmp(t, "1") == 0;
    free(line);
    return ok;
}

static void refresh(Menu *m)
{
    FileList next;
    filelist_init(&next);
    if (api_list(m->c, m->parent, &next)) {
        out("失败: %s\n", api_error());
        filelist_free(&next);
        return;
    }
    filelist_free(&m->items);
    m->items = next;
    print_list(m->display, &m->items);
}

static void push_parent(Menu *m, const char *p)
{
    if (m->nstack == m->capstack) {
        size_t cap = m->nstack ? m->nstack * 2 : 8;
        char **v = (char **)realloc(m->stack, cap * sizeof(char *));
        if (!v) return;
        m->stack = v; m->capstack = cap;
    }
    m->stack[m->nstack++] = str_dup(p);
}

static void enter(Menu *m, const char *id, const char *name)
{
    char *nd;
    push_parent(m, m->parent);
    free(m->parent);
    m->parent = str_dup(id);
    nd = join_path(m->display, name);
    free(m->display);
    m->display = nd;
    refresh(m);
}

static void up(Menu *m)
{
    char *last;
    if (m->nstack > 0) {
        m->nstack--;
        free(m->parent);
        m->parent = m->stack[m->nstack];
        m->stack[m->nstack] = NULL;
    } else {
        free(m->parent);
        m->parent = str_dup("root");
    }
    last = strrchr(m->display, '/');
    if (last) *last = 0;
    else { free(m->display); m->display = str_dup(""); }
    refresh(m);
}

static void download_one(Menu *m, const FileItem *it)
{
    DownloadResult res;
    char *name = safe_name(it->file_name);
    char *hs = human(it->size);
    char *disp = NULL;
    Buf nb;
    if (it->type == 1) {
        out("暂不支持文件夹打包下载: %s（请用网页端）\n", it->file_name);
        free(name); free(hs);
        return;
    }
    if (buf_init(&nb, 128) == 0) {
        buf_append_cstr(&nb, name ? name : "");
        disp = buf_take(&nb);
    }
    out("开始下载: %s (%s) -> %s\n", disp ? disp : "", hs ? hs : "", m->outdir);
    memset(&res, 0, sizeof res);
    if (download_file(m->c, it, m->outdir, &res)) {
        out("失败: %s — %s\n", disp ? disp : "", download_error());
        log_only("ERROR", download_error());
    } else if (res.skipped) {
        out("跳过: %s（已存在且大小一致）\n", disp ? disp : "");
    } else {
        char *hb = human(res.bytes);
        out("完成: %s (%s) -> %s\n", disp ? disp : "", hb ? hb : "", res.path);
        free(hb);
    }
    download_result_free(&res);
    free(disp); free(name); free(hs);
}

static void menu_download(Menu *m, const char *arg)
{
    int n = is_index(arg);
    if (n) {
        if ((size_t)n > m->items.n) { out("输入错误\n"); return; }
        download_one(m, &m->items.v[n - 1]);
        return;
    }
    {
        Resolver r;
        FileList hits;
        char *display = NULL;
        size_t i;
        r.c = m->c;
        filelist_init(&hits);
        if (resolve_select(&r, arg, "", 0, 1, &hits, &display)) {
            out("失败: %s\n", resolve_error());
            free(display);
            filelist_free(&hits);
            return;
        }
        for (i = 0; i < hits.n; i++) download_one(m, &hits.v[i]);
        free(display);
        filelist_free(&hits);
    }
}

static void menu_cd(Menu *m, const char *arg)
{
    int n;
    size_t k, cnt = 0;
    char **segs;
    char *parent = NULL, *display = NULL;
    FileList items;
    Resolver r;
    if (strcmp(arg, "..") == 0) { up(m); return; }
    if (strcmp(arg, "/") == 0 || strcmp(arg, "\\") == 0) {
        for (k = 0; k < m->nstack; k++) free(m->stack[k]);
        m->nstack = 0;
        free(m->parent); m->parent = str_dup("root");
        free(m->display); m->display = str_dup("");
        refresh(m);
        return;
    }
    n = is_index(arg);
    if (n) {
        if ((size_t)n > m->items.n || m->items.v[n - 1].type != 1) {
            out("输入错误：不是文件夹\n");
            return;
        }
        enter(m, m->items.v[n - 1].id, m->items.v[n - 1].file_name);
        return;
    }
    r.c = m->c;
    segs = split_selector(arg, &cnt);
    filelist_init(&items);
    if (resolve_dir(&r, segs, cnt, &parent, &display, &items)) {
        out("失败: %s\n", resolve_error());
    } else {
        push_parent(m, "root");
        free(m->parent); m->parent = parent; parent = NULL;
        free(m->display); m->display = display; display = NULL;
        filelist_free(&m->items);
        m->items = items;
        filelist_init(&items);
        print_list(m->display, &m->items);
    }
    free(parent); free(display);
    filelist_free(&items);
    split_selector_free(segs, cnt);
}

static void menu_link(Menu *m, const char *arg)
{
    int n = is_index(arg);
    char *url = NULL;
    if (!n || (size_t)n > m->items.n) { out("输入错误\n"); return; }
    if (api_download_url(m->c, &m->items.v[n - 1], &url)) {
        out("失败: %s\n", api_error());
        return;
    }
    out("链接含临时签名，注意保密：\n%s\n", url);
    free(url);
}

static void menu_login(Menu *m)
{
    if (api_login(m->c, m->cfg, m->cfg_path)) out("登录失败: %s\n", api_error());
    else refresh(m);
}

static int dispatch(Menu *m, char *line)
{
    if (strcmp(line, "exit") == 0 || strcmp(line, "q") == 0) return 1;
    if (strcmp(line, "help") == 0) { print_menu_help(); return 0; }
    if (strcmp(line, "ls") == 0 || strcmp(line, "re") == 0) { refresh(m); return 0; }
    if (strcmp(line, "login") == 0) { menu_login(m); return 0; }
    if (strcmp(line, "cd") == 0) { out("当前目录: %s\n", dir_label(m->display)); return 0; }
    if (strcmp(line, "..") == 0) { up(m); return 0; }
    if (strncmp(line, "download ", 9) == 0) { menu_download(m, trim(line + 9)); return 0; }
    if (strncmp(line, "cd ", 3) == 0) { menu_cd(m, trim(line + 3)); return 0; }
    if (strncmp(line, "link ", 5) == 0) { menu_link(m, trim(line + 5)); return 0; }
    {
        int n = is_index(line);
        if (n) {
            FileItem *it;
            if ((size_t)n > m->items.n) { out("输入错误\n"); return 0; }
            it = &m->items.v[n - 1];
            if (it->type == 1) {
                enter(m, it->id, it->file_name);
            } else {
                char *hs = human(it->size);
                out("%s  %s\n", it->file_name, hs ? hs : "");
                free(hs);
                if (confirm("输入 1 开始下载")) download_one(m, it);
            }
            return 0;
        }
    }
    out("未知命令，输入 help 查看\n");
    return 0;
}

int run_menu(Client *c, Options *o, const char *outdir,
             const char *cfg_path, Config *cfg)
{
    Menu m;
    size_t i;
    int code = 0;
    memset(&m, 0, sizeof m);
    m.c = c;
    m.o = o;
    m.cfg = cfg;
    m.cfg_path = cfg_path;
    m.outdir = outdir;
    m.parent = str_dup("root");
    m.display = str_dup("");
    filelist_init(&m.items);
    refresh(&m);
    out("输入 help 查看命令\n> ");
    for (;;) {
        char *line = NULL, *t;
        if (!read_line(&line)) break;
        t = trim(line);
        if (g_interrupted) { free(line); code = 1; break; }
        if (*t && dispatch(&m, t)) { free(line); break; }
        out("> ");
        free(line);
    }
    for (i = 0; i < m.nstack; i++) free(m.stack[i]);
    free(m.stack);
    free(m.parent); free(m.display);
    filelist_free(&m.items);
    return code;
}
