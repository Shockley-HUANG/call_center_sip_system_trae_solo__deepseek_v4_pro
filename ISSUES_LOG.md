# 项目问题记录 (Issue Log)

## 记录说明

本文档用于记录 `call_center_sip_system` 项目开发过程中遇到的各类技术问题、根因分析和解决方案，作为项目知识库的一部分，留存备用。

---

## 问题 #1  Git 文件修改追踪异常

**日期**：2026-05-18

**严重程度**：中

**状态**：已解决

### 问题描述

在修改 README.md 文件时，使用 `SearchReplace` 工具对同一文件执行了 7 次增量修改，每次工具都返回修改成功。但在提交阶段出现异常：
1. `git diff HEAD -- README.md` 输出为空
2. `git status` 显示 "nothing to commit, working tree clean"
3. 远程 raw.githubusercontent.com 拉取到的 README.md 仍然是旧版本

### 根因分析

在 Windows + PowerShell 终端环境下，对同一文件执行多次 `SearchReplace` 增量修改后，Git 的索引与工作区文件内容出现不同步。通过 `git hash-object` 对比验证，确认工作区文件哈希 不等于 索引/HEAD 哈希，文件确实已修改但 Git 未追踪。

### 最终解决方案

**方法一**：使用 `Write` 工具一次性完整重写整个文件（替代多次 SearchReplace）。

**方法二（更可靠）**：`git update-index --really-refresh <file>` 强制 Git 重新读取文件内容，无视 stat 缓存。

完整流程：
```powershell
Write <file>                                 # 写入文件
git update-index --really-refresh <file>     # 强制 Git 重新扫描
git status                                   # 确认检测到 modified
git add <file>                               # 暂存
git commit -m "..."                          # 提交
git push origin main                         # 推送
```

### 经验教训

1. 对同一文件做 >= 3 处修改时，用 Write 一次性写入
2. 文件修改后立即用 `git diff` 验证
3. 终极方案：`git update-index --really-refresh <file>`

---

## 问题 #2  GitHub 页面中文乱码（UTF-8 BOM 缺失）

**日期**：2026-05-18

**严重程度**：高

**状态**：已解决

### 问题描述

README.md 文件在本地 IDE 和 raw.githubusercontent.com 中中文显示正常，但在 GitHub 仓库主页渲染时全部显示为乱码。

### 根因分析

GitHub 主页的 Markdown 渲染引擎依赖 BOM（Byte Order Mark，文件头 `EF BB BF` 三字节）来自动检测文件编码。`Write` 工具默认写入 UTF-8 without BOM，导致 GitHub 无法正确识别中文。

### 最终解决方案

```powershell
$content = Get-Content <file> -Raw -Encoding UTF8
[System.IO.File]::WriteAllText("$PWD\<file>", $content, [System.Text.UTF8Encoding]::new($true))
# $true 参数 = 写入 BOM 前缀 EF BB BF
```

验证 BOM：
```powershell
$bytes = [System.IO.File]::ReadAllBytes("$PWD\<file>")
# 期望：BOM: 239 187 191
```

### 经验教训

所有含中文内容的项目文件写入后必须补写 UTF-8 BOM。

---

## 问题 #3  PowerShell to WSL 终端中文输出乱码

**日期**：2026-05-19

**严重程度**：中

**状态**：已解决

### 问题描述

在 Windows PowerShell 终端通过 `wsl -e bash -c "..."` 或 `wsl bash` 运行编译后的 `build/sip_server` 时，程序中所有中文字符输出均显示为乱码。

### 根因分析

PowerShell to WSL 管道编码不匹配（GBK 936 vs UTF-8）。

### 最终解决方案

在 WSL 内执行程序时设置 `LANG=zh_CN.UTF-8`。

### 经验教训

1. 所有通过 WSL 运行的程序，如果涉及中文输出，必须加 `LANG=zh_CN.UTF-8` 前缀
2. Makefile 中的 `run` / `demo` 目标已永久固化此设置

---

## 问题 #4  Write 工具隐式写入 UTF-8 BOM 导致重复 BOM

**日期**：2026-05-20

**严重程度**：高

**状态**：已解决

### 问题描述

V0.3 迭代中，使用 Write 工具写入新文件后，再用 PowerShell 补写 BOM，结果编译时报错：
```
error: expected '...' before 'typedef'
```

xxd 查看发现文件头出现双重 BOM（`EF BB BF EF BB BF`），GCC 将第二个 BOM 当作源代码的一部分解析，导致语法错误。

### 根因分析

Write 工具实际行为是**隐式自动写入 UTF-8 BOM**（与之前理解的不一致）。当再用 PowerShell `Get-Content -Encoding UTF8` 读取（自动剥离 BOM）+ `WriteAllText(UTF8Encoding($true))` 写入时，产生了双重 BOM。

部分文件甚至出现了三重 BOM（main.c 累积了 3 个）。

### 最终解决方案

编写 BOM 修复脚本，先检测并剥离所有重复 BOM，再写入单个 BOM：

```powershell
$bytes = [System.IO.File]::ReadAllBytes($path)
$start = 0
while ($start+2 -lt $bytes.Length -and
       $bytes[$start] -eq 239 -and
       $bytes[$start+1] -eq 187 -and
       $bytes[$start+2] -eq 191) {
    $start += 3
}
$content = [System.Text.Encoding]::UTF8.GetString($bytes, $start, $bytes.Length - $start)
[System.IO.File]::WriteAllText($path, $content, [System.Text.UTF8Encoding]::new($true))
```

### 经验教训

1. **Write 工具会隐式写入 BOM**，后续不需要再手动补写
2. 如需手动控制 BOM，先用上述脚本剥离重复 BOM，再精确写入一个
3. 编译失败时先用 `xxd <file> | head -3` 检查文件头十六进制

---

## 问题 #5  Lua 脚本文件不能包含 BOM

**日期**：2026-05-20

**严重程度**：中

**状态**：已解决

### 问题描述

Demo 模式运行时报错：
```
Failed to load Lua script [lua/route.lua]: unexpected symbol near '<\239>'
```

239 = 0xEF，即 BOM 的第一字节。Lua 解析器将 BOM 视为非法字符。

### 根因分析

与问题 #4 相同，Write 工具隐式写入了 BOM。C 编译器（GCC）能正确处理 BOM，但 Lua 解释器不能。

### 最终解决方案

对 `.lua` 文件使用不带 BOM 的方式写入：

```powershell
[System.IO.File]::WriteAllText($path, $content, [System.Text.UTF8Encoding]::new($false))
```

### 经验教训

- C 源文件（.c/.h）：需要 BOM（GitHub 渲染兼容） -> `UTF8Encoding($true)`
- Lua 脚本（.lua）：不能有 BOM（Lua 解析器不兼容） -> `UTF8Encoding($false)`

---

## 问题 #6  epoll 代码审查四项缺陷

**日期**：2026-05-20

**严重程度**：高

**状态**：已解决

### 问题描述

对 V3.0 epoll 骨架代码进行系统性审查，发现以下四项缺陷：

1. **ET 模式缺失**：未启用 `EPOLLET`，read 只执行一次而非 `while(1)` 循环读到 EAGAIN
2. **EPOLLOUT 空壳**：`default_on_write()` 仅取消 EPOLLOUT 防止 CPU 100%，但没有发送缓冲区和动态注册逻辑
3. **槽位 FD 复用隐患**：`connection_t` 无收发缓冲区字段，一旦后续添加，关闭旧连接时未清理会导致新连接读到脏数据
4. **UDP 丢包截断风险**：UDP Socket 使用 `read()` 代替 `recvfrom()`，无法获取发送方地址；缓冲区仅 1024 字节，SIP 大消息会截断

### 根因分析

V3.0 骨架阶段优先完成了框架搭建和编译验证，默认回调函数仅作为占位实现，未按生产标准完善。

### 最终解决方案

**问题1  ET 模式循环读**：

所有 Socket 注册均添加 `EPOLLET`，TCP 用 `while(1) read` 直到 EAGAIN，UDP 用 `while(1) recvfrom` 直到 EAGAIN。

**问题2  EPOLLOUT 按需调度**：

`connection_t` 新增 `send_buf` / `send_len` / `send_offset` 字段。`el_send_data()` 先尝试立即发送，写不完则暂存并注册 EPOLLOUT；`default_on_write()` 续发剩余数据，发完立即取消 EPOLLOUT。

**问题3  槽位安全清理**：

`free_connection()` 显式清零 `send_len`、`send_offset`，并 `memset(send_buf, 0, ...)`。

**问题4  UDP recvfrom + 大缓冲区**：

缓冲区扩至 65536 字节，UDP 分支使用 `recvfrom()` 替代 `read()`，获取发送方地址。

同时新增 `es_send_all()` 底层发送引擎，支持 `send()`/`sendto()` 统一接口，处理 `EAGAIN`/`EINTR`/部分写入。

### 经验教训

1. ET 模式是高并发服务端的刚需，骨架阶段就应纳入
2. EPOLLOUT 必须"注册-发送-取消"完整闭环，漏了"取消"会 CPU 100%
3. 结构体字段 `memset` 清零 + 缓冲区显式清理双保险
4. UDP 必须用 `recvfrom`（需地址）且缓冲区 >= 65536 字节

---

## 问题 #7  IDE 自动追加 UTF-8 BOM 导致 GCC 编译报错

**日期**：2026-05-21

**严重程度**：高

**状态**：已解决

### 问题描述

在迭代4开发过程中，IDE（Trae Solo）每次对 `.c` / `.h` 文件执行 `Write` 或 `SearchReplace` 操作后，会自动在文件开头追加 UTF-8 BOM（`EF BB BF`）。GCC 编译器不支持 C 源文件开头的 BOM 字节，导致编译报错：

```
error: expected ';' before 'typedef'
    1 | ﻿/*
      | ^
      | ;
```

反复尝试手动剥离 BOM 后，IDE 再次编辑文件时又会自动追加，形成死循环。

### 根因分析

项目规范要求 `.md` 文件中使用 UTF-8 BOM 以兼容 GitHub 中文渲染，但 `Write` 工具的 `UTF8Encoding($true)` 模式对所有文件都追加 BOM。C/C++ 编译器（GCC/Clang）不认识 BOM 标记，将其视为非法源代码字符。

### 最终解决方案

**方案一（Makefile 层自动剥离）**：在 Makefile 的 `all` 目标前置 `strip-bom` 步骤，使用 Python3 脚本在每次编译前自动扫描 `src/*.c` 和 `include/*.h`，检测并剥离 BOM 字节。确保编译过程零人工干预。

```python
for f in glob.glob("src/*.c") + glob.glob("include/*.h"):
    d = open(f, "rb").read()
    if d[:3] == b"\xef\xbb\xbf":
        open(f, "wb").write(d[3:])
```

**方案二（编译流程链）**：`all: check-lua dirs strip-bom $(TARGET)`，确保 BOM 剥离在 GCC 编译之前执行。

### 经验教训

1. `.c` / `.h` 源文件不能有 BOM，GCC 不支持；`.md` 文档需要 BOM 以兼容 GitHub 渲染
2. Makefile 本身也不能有 BOM（GNU Make 不认识）
3. Lua 脚本（`.lua`）不能有 BOM（Lua 解释器不兼容）
4. 编译流程前置自动化清理是比手动反复修复更可靠的方案

---

## 问题 #8  Git 提交流程三连故障（索引不同步 / 身份丢失 / Push 认证）

**日期**：2026-05-21

**严重程度**：中

**状态**：已解决（已固化到项目规则中，后续直接按固化流程执行）

### 问题描述

迭代4 提交阶段，Git 操作连续遭遇三类故障，导致 commit 耗时远超预期：

1. **Git 索引不同步（问题 #1 复发）**：`README.md`、`ISSUES_LOG.md` 被多次 `SearchReplace` 修改后，`git status` 不显示这两个文件，需要 `git update-index --really-refresh` + `git add -f` 强制处理
2. **Git 用户身份丢失**：`git commit` 时报 `Author identity unknown`，`git config user.name` 返回空。根因是 WSL 重启后仓库级 `.git/config` 无用户信息，需重新 `git config user.name/email`
3. **Push 交互式认证**：`git push origin main` 停在 `Username for 'https://github.com':`，HTTPS 远程仓库无法在非交互终端自动认证，必须用户手动输入

### 根因分析

| 故障 | 根因 |
|------|------|
| 索引不同步 | 同一文件多次增量编辑后，Git stat 缓存与实际内容脱节（已知问题 #1） |
| 身份丢失 | WSL 终端重启后，`git config --local` 的 user.name/email 可能被清空，且全局 `~/.gitconfig` 也不可靠 |
| Push 认证 | HTTPS 远程仓库的 credential 缓存过期，非交互终端无法弹出输入提示 |

### 最终解决方案

**对整个提交流程进行了标准化固化**（已写入 `project_rules.md` + `ai_dev_constraints.md`）：

1. **Git 预设身份**：每次 commit 前强制 `git config user.name/email`，不依赖 WSL 持久化
2. **强制刷新索引**：所有被修改的 `.md` 文件，commit 前统一 `git update-index --really-refresh` → `git add -f`
3. **Push 独立执行**：commit 与 push 分离，push 失败时直接提示用户手工执行，不重试、不卡死
4. **`git add -A` 禁用**：改用精确文件列表 add，避免 `tools/` 等临时目录误提交

### 经验教训

1. Git 在 Windows+WSL 混合环境下不可靠 —— 不要相信 `git status` 的即时准确性，多用 `--really-refresh`
2. 用户身份配置不能依赖 WSL 持久化 —— 每次 commit 前强制设定
3. HTTPS Push 是唯一无解的问题 —— 遇到立即告知用户手工处理，不做徒劳重试

---
## 问题 #9  V4.2 编译错误三连（ETIMEDOUT/旧常量/前向声明）

**日期**：2026-05-30

**严重程度**：低

**状态**：已解决

### 问题描述

迭代5 V4.2 代码首次编译时遇到三个错误，均为头文件依赖不完整所致：

1. **`ETIMEDOUT` undeclared**：`db_mysql.c` 的 `pool_borrow()` 中使用了 `pthread_cond_timedwait()` 返回值比较 `ETIMEDOUT`，但未包含 `<errno.h>`。`ETIMEDOUT` 是 POSIX 错误码宏，定义在 `<errno.h>` 中。

2. **`DB_RECONNECT_INTERVAL_SEC` undeclared**：`db_redis.c` 仍引用旧版常量名。V2.0 架构升级时 `db_config.h` 将重连配置从 `DB_RECONNECT_INTERVAL_SEC` 重构为 `DB_RECONNECT_BASE_SEC`，但 `db_redis.c` 未同步更新。

3. **`MYSQL` 类型前向声明缺失**：`db_config.h` 中 `mysql_slot_t` 结构体包含 `MYSQL *conn` 指针字段，但 `MYSQL` 类型仅在 `<mysql/mysql.h>` 中声明。由于 `db_config.h` 被 `db_mysql.h` 在 `<mysql/mysql.h>` 之前 include，导致编译时 `MYSQL` 未定义。

### 根因分析

| 错误 | 根因 |
|------|------|
| ETIMEDOUT | `pthread_cond_timedwait` 返回值比较需要 `<errno.h>`，未包含 |
| DB_RECONNECT_INTERVAL_SEC | V2.0 架构升级时 `db_config.h` 常量重命名，`db_redis.c` 遗漏更新 |
| MYSQL 前向声明 | `mysql_slot_t` 定义在 `db_config.h`（MYSQL未知），应移至 `db_mysql.h`（MYSQL已知） |

### 解决方案

1. `db_mysql.c` 增加 `#include <errno.h>`
2. `db_redis.c` 将 `DB_RECONNECT_INTERVAL_SEC` 替换为 `DB_RECONNECT_BASE_SEC`
3. 将 `mysql_slot_t` 定义从 `db_config.h` 移至 `db_mysql.h`（`<mysql/mysql.h>` 之后）

### 经验教训

1. 使用 `pthread_cond_timedwait` 时必须显式包含 `<errno.h>` 以获取 `ETIMEDOUT`
2. 重构常量名时需全局搜索替换，避免遗漏
3. 依赖外部库类型（`MYSQL`）的结构体应定义在对应的模块头文件中，而非通用配置头文件
4. 代码审查前的实际编译验证不可替代

---

## 问题记录模板

> 后续新问题按以下格式追加：
>
> ```
> ## 问题 #N  简要描述
> 
> **日期**：YYYY-MM-DD
> **严重程度**：高 / 中 / 低
> **状态**：已解决 / 修复中 / 待处理
> 
> ### 问题描述
> ### 根因分析
> ### 解决方案
> ### 经验教训
> ```