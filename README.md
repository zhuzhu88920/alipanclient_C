# alipanclient_C

阿里云盘命令行客户端（C99 单文件）：列目录 + 下载 + 序号选择器，扫码授权，分享接口预留。

Windows x64，零第三方库，仅依赖系统 DLL（WinHTTP / Shell32 / msvcrt）。

## 项目结构

| 文件 | 说明 |
|---|---|
| `main.c` | 程序入口：参数解析、登录引导、CLI / 交互菜单分流 |
| `pan.h` | 公共结构体、常量与函数声明 |
| `api.c` | 阿里云盘 Open API（列目录 / 下载链接）+ tickstep 中转扫码授权；`api_share` 预留 |
| `config.c` | 配置文件读写（`alipan_config.json`） |
| `json.c` | 轻量 JSON 解析与转义（自实现） |
| `http.c` | WinHTTP 封装：普通请求、Range 流式读取 |
| `download.c` | 分片并发下载（最多 3 片）、断点续传、Range 不支持时回退 |
| `resolve.c` | 文件列表缓存与选择器（序号 / 模糊 / 通配 / 多层子目录） |
| `cli.c` | 命令行参数解析、帮助文本、一行命令执行 |
| `menu.c` | 交互菜单 |
| `progress.c` / `out.c` / `log.c` / `util.c` | 进度条、控制台输出、日志、字符串与 Buf 工具 |
| `sign.c` | 从 123panclient_C 复制的遗留文件，**不参与编译**（阿里云盘无需该签名） |
| `build.ps1` | MinGW-w64 编译脚本 |
| `config.example.json` | 配置模板（全空值，无敏感信息） |
| `alipan_config.json` | 运行时生成，含真实 token；已被 `.gitignore` 排除，**绝不提交** |
| `tests/mockserver.py` | 本地协议 mock（列目录 / 3 分片下载 / 续传 / Range 回退） |
| `tests/config.mock.json` | mock 专用假 token，可安全提交 |

## 使用方法

### 1. 首次授权

在 CMD 中执行（不要双击，便于看报错）：

```
cd /d D:\code\alipanclient_C
alipanclient_C.exe
```

程序拉起浏览器打开授权页 → 完成一次 OAuth 授权 + 一次 APP 扫码 → 回控制台按 Enter → token 存入同目录 `alipan_config.json`。后续启动免扫码。

### 2. 命令行

```
alipanclient_C.exe                          交互菜单（首次自动扫码）
alipanclient_C.exe login                    重新扫码登录
alipanclient_C.exe list                     列根目录
alipanclient_C.exe list --in "photo"        列子目录
alipanclient_C.exe download 7               下载根目录第 7 项
alipanclient_C.exe download "OS/3"          下载 OS 目录第 3 项
alipanclient_C.exe download "win*.iso" --all
alipanclient_C.exe download 7 --out "D:\dl"
alipanclient_C.exe download 7 --config "D:\other\alipan_config.json"
alipanclient_C.exe --help
```

**选择器**

| 写法 | 含义 |
|---|---|
| `7` | 当前目录第 7 项（序号来自 `list` 输出） |
| `document.pdf` | 按文件名匹配（忽略大小写、支持子串） |
| `"OS/3"` | 子目录 `OS` 下第 3 项 |
| `"design soft/xx.exe"` | 含空格的多层路径 |
| `"OS/win*.iso"` | 通配符，多命中需加 `--all` |

**选项**

| 选项 | 说明 |
|---|---|
| `--in "OS"` | 指定目录（等价于选择器路径前缀） |
| `--out "D:\xxx"` | 自定义下载目录 |
| `--all` | 多命中时全部下载 |
| `--config <path>` | 指定配置文件 |
| `--verbose` | 输出附加信息 |
| `-h` / `--help` | 帮助 |
| `--dir --yes` | 参数已解析，但**文件夹打包下载尚未实现**，请勿用于目录 |

### 3. 交互菜单

| 命令 | 说明 |
|---|---|
| `ls` / `re` | 显示 / 刷新当前目录 |
| `<数字>` | 进入文件夹，或选中文件后输入 `1` 下载 |
| `download <序号|路径>` | 下载（路径相对根目录） |
| `cd <序号|..|/|路径>` | 切换目录 |
| `link <序号>` | 打印该文件的临时下载链接（含凭据，注意保密） |
| `login` | 重新扫码登录 |
| `help` / `exit`（或 `q`） | 帮助 / 退出 |

### 4. 下载与续传

- ≥4 MB 分 3 片、≥512 KB 分 2 片，各片独立 `Range` 连接并发下载，速度约为单连接的 3 倍。
- 中断后同目录留下 `文件名.part` 与 `.p0/.p1/.p2` 位点文件；重跑同一文件自动续传，完成后自动清理。**不要手动删除或改小这些文件**。
- 服务端不支持 `Range` 时自动回退单连接下载。

## 方案 A 说明

授权走 tickstep 中转服务（`api.tickstep.com`）换取 token，Open API 直连 `openapi.alipan.com`。

- 优点：无需自己申请开放平台应用，扫码即可用。
- 代价：依赖第三方中转服务可用性；token 刷新未实现，过期后重新 `login`。
- 3 片并发是阿里云盘官方上限，再多线程无效；大文件速度仍受限于单连接限速。

## 脱敏说明

- 仓库内**不含任何真实 token、ticket、账号或隐私数据**。真实凭据只存在于本地 `alipan_config.json`，已被 `.gitignore` 排除。
- `config.example.json` 全为空值；`tests/config.mock.json` 使用 `mock-token` / `mock-ticket` 假值。
- `api.c` 中的 `CLIENT_ID` / `AUTH_KEY` 是 tickstep 开源项目的公开常量（授权协议所需），不是个人密钥。
- 推送前请再次确认：`git status` 中无 `alipan_config.json`、`GitHub-token.txt`、`*.log`、`*.part`。

## 构建

工具链：MinGW-w64 GCC。默认使用 `D:\code\_tools\w64devkit\w64devkit\bin\gcc.exe`，不存在时回退到 PATH 中的 `gcc`。

```powershell
powershell -File build.ps1          # alipanclient_C.exe
powershell -File build.ps1 -Upx     # UPX 压缩
powershell -File build.ps1 -Gcc "C:\path\to\gcc.exe"
```

## 测试

`tests/mockserver.py` 提供本地协议 mock（不依赖真实账号）：

```powershell
python tests\mockserver.py 18765
$env:ALIPAN_API_BASE = 'http://127.0.0.1:18765'
.\alipanclient_C.exe list --config tests\config.mock.json
```

`ALIPAN_API_BASE` / `ALIPAN_TOKEN_HOST` 仅用于测试指向 mock，正式使用不要设置。

## 排障

- **扫码授权后按 Enter 窗口闪退 / 没生成 `alipan_config.json`**：旧版本 URL 缓冲区复用 + JSON 嵌套对象解析错位，已修复。
- **登录成功但列目录报 HTTP 400**：旧版本拼 JSON 请求体时值没加引号，已修复。
- **双击 exe 看不到错误信息**：在 CMD 里执行 `alipanclient_C.exe login`。
- **提示 token 过期**：重新执行 `login` 扫码（当前未实现自动刷新）。
- **下载中断后看到 `.part.p0/.p1/.p2`**：分片续传位点，正常现象，重跑同一文件自动续传。

## 免责

本工具用于个人管理自有阿里云盘文件，请遵守服务条款。
