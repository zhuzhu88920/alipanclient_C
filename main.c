#include "pan.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_interrupted, 1);
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    Options o;
    Config cfg;
    Client c;
    char *edir = NULL, *cfgpath = NULL, *outdir = NULL;
    Buf b;
    int code = 0, rc;

    memset(&o, 0, sizeof o);
    memset(&cfg, 0, sizeof cfg);
    memset(&c, 0, sizeof c);
    out_init();
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    if (parse_args(argc, argv, &o)) {
        out("参数错误: %s\n", args_error());
        return 2;
    }
    if (o.help || (o.command && strcmp(o.command, "help") == 0)) {
        print_help();
        options_free(&o);
        return 0;
    }

    edir = exe_dir();
    if (o.config) cfgpath = str_dup(o.config);
    else {
        if (buf_init(&b, 256)) { code = 1; goto done; }
        buf_printf(&b, "%s\\alipan_config.json", edir);
        cfgpath = buf_take(&b);
    }
    log_init(edir);
    rc = config_load(cfgpath, &cfg);
    if (rc != 0) {
        out("config.json 解析失败: %s\n", cfgpath);
        code = 2;
        goto done;
    }
    outdir = o.out ? str_dup(o.out) : default_download_dir(cfg.download_path);
    if (o.verbose) out("输出目录: %s\n", outdir);

    /* login 命令：显式登录 */
    if (o.command && strcmp(o.command, "login") == 0) {
        if (client_init(&c, &cfg, o.verbose)) { out("初始化失败\n"); code = 1; goto done; }
        if (api_login(&c, &cfg, cfgpath)) {
            out("登录失败: %s\n", api_error());
            code = 1;
        }
        goto done;
    }

    /* 未登录：自动引导扫码 */
    if (!cfg.access_token || !*cfg.access_token) {
        out("尚未登录，开始扫码授权。\n");
        if (client_init(&c, &cfg, o.verbose)) { code = 1; goto done; }
        if (api_login(&c, &cfg, cfgpath)) {
            out("登录失败: %s\n", api_error());
            code = 1;
            goto done;
        }
    } else {
        if (client_init(&c, &cfg, o.verbose)) { out("初始化失败\n"); code = 1; goto done; }
    }

    code = o.command ? run_cli(&c, &o, outdir) : run_menu(&c, &o, outdir, cfgpath, &cfg);

done:
    client_free(&c);
    config_free(&cfg);
    options_free(&o);
    free(edir); free(cfgpath); free(outdir);
    log_close();
    return code;
}
