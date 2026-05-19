# 项目规则 — 呼叫中心 SIP 系统

## 版本管控规范

### Commit 前强制检查清单（必须逐项执行，缺一不可）

**每次 `git commit` 之前，必须完成以下全部检查并更新对应文件：**

| # | 检查项 | 目标文件 | 操作 |
|---|--------|----------|------|
| 1 | 开发进展追加 | **README.md** | 在「开发进展」表格**最顶部追加一行**（时间 + 版本 + 内容），禁止覆盖已有行 |
| 2 | 任务状态同步 | **tasks.md** | 将本次已完成任务从「进行中」移至「已完成」，标注日期；新增条目到对应分类 |
| 3 | 问题记录补充 | **ISSUES_LOG.md** | 若本次涉及新问题修复，追加完整问题记录（描述/根因/方案/教训） |
| 4 | 设计文档同步 | **docs/*.md** | 若本次涉及架构/接口/业务规则变更，必须同步更新版本号和内容 |
| 5 | UTF-8 BOM 验证 | 所有含中文文件 | 执行 PowerShell BOM 补写脚本，确认前3字节均为 `239 187 191` |
| 6 | Git 变更确认 | `git status` | 确认所有变更文件已 `git add`，无遗漏；确认无意外文件被修改 |

**执行顺序**：先完成 1-5 的文件更新 → 再执行 6 确认 → 最后 commit + push。

**违规后果**：若 commit 后发现有文件未更新，必须立即补更并重新 commit，禁止拖延到下一个迭代。

### Commit 信息格式

`feat:` / `fix:` / `docs:` / `refactor:` + 简要描述

### 远程仓库

https://github.com/Shockley-HUANG/call_center_sip_system_trae_solo__deepseek_v4_pro

## 文件编码规范（重要）

### 规则：所有含中文内容的文件必须使用 UTF-8 with BOM 编码

**原因**：GitHub 主页渲染引擎依赖 BOM（Byte Order Mark）前缀来自动检测文件编码。如果文件为 UTF-8 without BOM，GitHub 无法正确识别中文内容，会导致页面显示乱码。

| 编码格式 | GitHub 主页 | raw 文件 | 本地 IDE |
|----------|:-----------:|:--------:|:--------:|
| UTF-8 with BOM | ✅ 正常 | ✅ 正常 | ✅ 正常 |
| UTF-8 without BOM | ❌ 乱码 | ✅ 正常 | ✅ 正常 |

**适用文件**：
- README.md
- ISSUES_LOG.md
- docs/ 目录下的所有 .md 文件
- 任何包含中文注释的源代码文件

**操作流程（写入文件后必须执行）**：

```powershell
# 读取文件内容
$content = Get-Content <file> -Raw -Encoding UTF8

# 以 UTF-8 with BOM 写入（关键步骤）
[System.IO.File]::WriteAllText("$PWD\<file>", $content, [System.Text.UTF8Encoding]::new($true))

# 验证 BOM（前3字节应为 239 187 191）
$bytes = [System.IO.File]::ReadAllBytes("$PWD\<file>")
Write-Output "BOM: $($bytes[0]) $($bytes[1]) $($bytes[2])"
```

**注意**：`Write` 工具默认写入 UTF-8 without BOM，写入后必须用上述 PowerShell 命令追加 BOM。

## 文件修改规范（重要）

### 规则：对同一文件做多处修改时，优先使用 Write 一次性完整重写

**原因**：在 PowerShell 终端环境下，对同一文件执行多次 `SearchReplace` 增量修改后，Git 的索引（index/staging area）可能无法感知文件变更，导致：
- `git diff` 输出为空
- `git status` 显示 "nothing to commit, working tree clean"
- 实际上磁盘文件内容已更新

**适用场景**：

| 场景 | 推荐方式 |
|------|----------|
| 修改文件中 **≥ 3 处** 不同位置 | `Write` 一次性完整重写整个文件 |
| 修改文件中 **1-2 处** 局部内容 | `SearchReplace` 增量修改 |
| 新增文件 | `Write` |

**操作流程**：
1. 先用 `Read` 读取文件全部内容
2. 在内存中完成所有修改
3. 用 `Write` 一次性写入完整文件
4. **用 PowerShell 补写 UTF-8 BOM**（见上方「文件编码规范」）
5. 立即执行 `git update-index --really-refresh <file>` + `git add <file>` + `git diff --cached` 验证暂存区

### 终极方案：Git 无法检测文件变更时

如果 `Write` 完整重写后 Git 仍无法检测到变更（`git status` 显示 clean）：

```powershell
# 对比工作区和 HEAD 的哈希，确认文件确实被修改
git hash-object <file>           # 工作区哈希
git ls-tree HEAD <file>          # HEAD 哈希

# 强制 Git 跳过 stat 缓存，重新扫描文件内容
git update-index --really-refresh <file>

# 此后 git status 应能检测到变更
git status
git add <file>
git commit -m "..."
git push origin main
```

**关键区别**：
- `git update-index --refresh`：仅刷新 stat 信息，如果 mtime 不变则跳过
- `git update-index --really-refresh`：**强制重新读取文件内容并计算哈希**，无视 stat 缓存
- `--really-refresh` 比修改文件 mtime（`LastWriteTime`）、`git add -f` 等方案都更可靠

## 终端中文编码规范（重要）

### 规则：WSL 运行项目时必须设置 `LANG=zh_CN.UTF-8` 环境变量

**原因**：在 Windows PowerShell 环境通过 WSL 运行 Linux 程序时，PowerShell 与 WSL 之间的管道传输默认使用系统编码（Windows 默认为 GBK/936），而 WSL 内程序输出为 UTF-8。编码不匹配导致所有中文字符在终端显示为乱码。

**现象**：
```
# 乱码示例（PowerShell → WSL 管道）
[DEBUG] Route dispatch: desc=閿€鍞儴 鏈夌┖闂插潗甯
[route.lua]  宸ヤ綔鏃ユ椂娈碉紝浣跨敤鏍囧噯璺敱

# 正常显示（WSL 内部 + LANG=zh_CN.UTF-8）
[DEBUG] Route dispatch: desc=销售部 有空闲坐席
[route.lua]  工作日时段，使用标准路由策略
```

**解决方案**：在 Makefile 的 `run` / `demo` 目标中添加 `LANG=zh_CN.UTF-8` 前缀：

```makefile
.PHONY: run
run: all
	@LANG=zh_CN.UTF-8 $(TARGET)

.PHONY: demo
demo: all
	@echo "a" | LANG=zh_CN.UTF-8 timeout 5 $(TARGET) 2>&1; exit 0
```

**手动运行方式**：
```bash
# 在 WSL 内直接运行（推荐）
LANG=zh_CN.UTF-8 ./build/sip_server

# 从 PowerShell 进入 WSL 后再运行
wsl -e bash -c "LANG=zh_CN.UTF-8 ./build/sip_server"
```

**重要提醒**：此规则永久生效。后续新增的任何涉及中文输出的运行命令，无论是 Makefile 目标还是手动命令，都必须包含 `LANG=zh_CN.UTF-8`。

## 构建与运行

- 编译：`make`（生成 `build/sip_server`）
- 运行：`make run`（自动设置 LANG=zh_CN.UTF-8）
- Demo：`make demo`（编译 + 运行6组测试，5秒后自动退出）
- 清理：`make clean` / `make distclean`
- 调试：`make debug` / `make gdb`

## 项目技术栈

- 语言：C11 + Lua 5.2/5.3/5.4
- 通信协议：SIP + RTP
- 并发模型：epoll
- 构建工具：GNU Make + GCC
- 开发工具：Trae Solo IDE + DeepSeek V4 PRO
