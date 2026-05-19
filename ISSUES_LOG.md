# 项目问题记录 (Issue Log)

## 记录说明

本文档用于记录 `call_center_sip_system` 项目开发过程中遇到的各类技术问题、根因分析和解决方案，作为项目知识库的一部分，留存备用。

---

## 问题 #1 — Git 文件修改追踪异常

**日期**：2026-05-18

**严重程度**：中

**状态**：已解决 ✅

### 问题描述

在修改 README.md 文件时，使用 `SearchReplace` 工具对同一文件执行了 7 次增量修改，每次工具都返回修改成功。但在提交阶段出现异常：
1. `git diff HEAD -- README.md` 输出为空
2. `git status` 显示 "nothing to commit, working tree clean"
3. 远程 raw.githubusercontent.com 拉取到的 README.md 仍然是旧版本

### 根因分析

在 Windows + PowerShell 终端环境下，对同一文件执行多次 `SearchReplace` 增量修改后，Git 的索引与工作区文件内容出现不同步。通过 `git hash-object` 对比验证，确认工作区文件哈希 ≠ 索引/HEAD 哈希，文件确实已修改但 Git 未追踪。

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

1. 对同一文件做 ≥ 3 处修改时，用 Write 一次性写入
2. 文件修改后立即用 `git diff` 验证
3. 终极方案：`git update-index --really-refresh <file>`

---

## 问题 #2 — GitHub 页面中文乱码（UTF-8 BOM 缺失）

**日期**：2026-05-18

**严重程度**：高

**状态**：已解决 ✅

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

## 问题 #3 — PowerShell → WSL 终端中文输出乱码

**日期**：2026-05-19

**严重程度**：中

**状态**：已解决 ✅

### 问题描述

在 Windows PowerShell 终端通过 `wsl -e bash -c "..."` 或 `wsl bash` 运行编译后的 `build/sip_server` 时，程序中所有中文字符输出均显示为乱码。例如：

```
# 乱码输出
[DEBUG] Route dispatch: desc=閿€鍞儴 鏈夌┖闂插潗甯
[route.lua]  宸ヤ綔鏃ユ椂娈碉紝浣跨敤鏍囧噯璺

# 正常应该是
[DEBUG] Route dispatch: desc=销售部 有空闲坐席
[route.lua]  工作日时段，使用标准路由策略
```

英文和数字输出完全正常，仅中文受影响。

### 根因分析

**编码链路不匹配**：

| 环节 | 编码 | 说明 |
|------|------|------|
| WSL 内程序 (sip_server) | UTF-8 | C/Lua 程序输出 |
| WSL bash 终端 | UTF-8 | WSL 内部原生 UTF-8 |
| PowerShell ← WSL 管道 | GBK (936) | PowerShell 默认使用系统 ANSI 编码 |
| PowerShell 控制台 | GBK (936) | Windows 中文版默认代码页 |

当输出链路为 **程序 → WSL bash → PowerShell 管道 → 终端** 时，UTF-8 字节流被 PowerShell 以 GBK 方式解码，多字节 UTF-8 中文字符（通常 3 字节）被错误拆解为 GBK 双字节序列，产生乱码。

**直接验证**：在 WSL 内部直接运行程序（不经过 PowerShell 管道），中文完全正常：
```bash
# WSL 内部运行 — 无乱码
LANG=zh_CN.UTF-8 ./build/sip_server
```

### 尝试过的无效操作

| 操作 | 结果 |
|------|------|
| `chcp 65001` 切换 PowerShell 代码页 | 无效（管道编码不受 `chcp` 影响） |
| `[Console]::OutputEncoding = UTF8` | 无效（Trae Sandbox 终端限制） |
| 程序内部 setlocale() | 无效（问题在传输管道，不在程序） |
| 直接 `wsl ./build/sip_server` | 依然乱码 |

### 最终解决方案

在 WSL 内执行程序时，通过 **WSL 内部管道** 完成输出，避免经过 PowerShell 的编码转换层。直接在 Makefile 的 `run` 和新增 `demo` 目标中设置 `LANG=zh_CN.UTF-8` 前缀：

```makefile
# Makefile 修改
.PHONY: run
run: all
	@LANG=zh_CN.UTF-8 $(TARGET)          # 之前: @cd $(CURDIR) && $(TARGET)

.PHONY: demo
demo: all
	@echo "a" | LANG=zh_CN.UTF-8 timeout 5 $(TARGET) 2>&1; exit 0   # 新增目标
```

**关键机制**：`LANG=zh_CN.UTF-8` 不仅设置程序输出的 locale，更重要的是**强制 WSL 子进程以 UTF-8 模式运行**，当 `timeout` 或管道输出回 PowerShell 时，WSL 会自动处理编码转换。

**手动运行的正确方式**：
```bash
# ✅ 正确（WSL 内）
wsl -e bash -c "LANG=zh_CN.UTF-8 ./build/sip_server"

# ✅ 正确（Makefile）
make run    # 已内置 LANG=zh_CN.UTF-8
make demo   # Demo 测试，5秒自动退出

# ❌ 错误（会出现乱码）
wsl ./build/sip_server
```

### 经验教训

1. **所有通过 WSL 运行的程序，如果涉及中文输出，必须加 `LANG=zh_CN.UTF-8` 前缀**
2. Makefile 中的 `run` / `demo` 目标已永久固化此设置
3. 不要在 PowerShell 侧尝试解决（chcp/OutputEncoding 在 Sandbox 环境无效），从 WSL 侧统一
4. 此规则写入 project_rules.md「终端中文编码规范」章节，后续所有新命令自动遵守
5. 新增 `make demo` 目标，方便快速验证而无需手动 Ctrl+C 退出

### 已落地规则

见 [.trae/rules/project_rules.md](./.trae/rules/project_rules.md) — 「终端中文编码规范」章节。

---

## 问题记录模板

> 后续新问题按以下格式追加：
>
> ```
> ## 问题 #N — 简要描述
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
