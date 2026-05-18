# 项目问题记录 (Issue Log)

## 记录说明

本文档用于记录 `call_center_sip_system` 项目开发过程中遇到的各类技术问题、根因分析和解决方案，作为项目知识库的一部分，留存备用。

---

## 问题 #1 — Git 文件修改追踪异常

**日期**：2026-05-18

**严重程度**：中

**状态**：已解决 ✅

### 问题描述

在修改 [README.md](./README.md) 文件时，使用 `SearchReplace` 工具对同一文件执行了 7 次增量修改（添加开发工具章节、修正中英文间距、修正括号间距等），每次工具都返回修改成功。但在提交阶段出现以下异常：

1. `git diff HEAD -- README.md` 输出为空
2. `git status` 显示 "nothing to commit, working tree clean"
3. 远程 raw.githubusercontent.com 拉取到的 README.md 仍然是旧版本

### 根因分析

在 Windows + PowerShell 终端环境下，对同一文件执行多次 `SearchReplace` 增量修改后，Git 的索引（index / staging area）与工作区文件内容出现不同步：

| 层级 | 状态 | 说明 |
|------|------|------|
| 磁盘文件 | ✅ 已更新 | `Write` / `SearchReplace` 直接写入磁盘成功 |
| Git stat 缓存 | ❌ 未刷新 | Git 基于 mtime/size 缓存，未感知到文件变更 |
| Git index | ❌ 未更新 | `git diff` 使用缓存的 stat 信息，认为文件未变 |

通过 `git hash-object` 对比验证，确认工作区文件哈希 (`7614ec`) ≠ 索引/HEAD 哈希 (`ac2d15`)，文件确实已修改但 Git 未追踪。

Windows 文件系统 API 与 Git 的 stat 缓存机制在特定条件下存在竞态，导致增量修改被 Git 忽略。

### 尝试过的无效操作

| 操作 | 结果 |
|------|------|
| `git diff HEAD -- README.md` | 输出为空 |
| `git update-index --refresh README.md` | 无效 |
| `git checkout HEAD -- README.md` | 文件未被恢复到旧版本 |
| `git reset HEAD README.md` | 无效 |
| `git add -f README.md` | 无效 |
| 多次 `git commit` 尝试 | "nothing to commit" |
| 修改文件 mtime (`LastWriteTime = Get-Date`) | 无效 |
| 用 `Write` 工具完整重写文件 | 无效（后续测试中发现） |

### 最终解决方案

**方法一（第一轮）**：使用 `Write` 工具一次性完整重写整个 README.md 文件（260 行），替代 7 次 `SearchReplace` 增量修改：

```powershell
Write README.md  # 一次性写入全部内容
git add README.md  # → 检测到 modified ✅
git commit -m "docs: README格式规范化，补充开发工具信息（Trae Solo + DeepSeek V4 PRO）"
git push -u origin main  # → 远程同步成功 ✅
```

最终 commit：`55809e1`。

---

**方法二（第二轮，更可靠）**：当 `Write` 工具也无法触发 Git 检测时（在后续修改中再次出现），使用以下命令**强制刷新 Git 索引**：

```powershell
git update-index --really-refresh <file>
```

关键区别：
- `git update-index --refresh`：仅刷新 stat 信息，如果 mtime 不变则跳过
- `git update-index --really-refresh`：**强制重新读取文件内容**，无视 stat 缓存

完整流程：
```powershell
Write README.md                                   # 写入文件
git update-index --really-refresh README.md       # 强制 Git 重新扫描文件
git status                                         # → 确认检测到 modified ✅
git add README.md                                  # 暂存
git commit -m "..."                                # 提交
git push origin main                               # 推送
```

最终 commit：`f8c8ddd`，SHA 与远程一致，GitHub 页面确认内容已同步。

### 经验教训

1. **对同一文件做 ≥ 3 处修改时，不要用 SearchReplace 逐次修改**，应先用 Read 读取全文，在内存中完成所有修改，再用 Write 一次性写入。
2. 文件修改后立即用 `git diff` 验证 Git 是否感知到变更。
3. 如果 `git diff` 为空但文件内容确实变了，使用 `git hash-object <file>` 对比工作区和 HEAD 的哈希确认差异。
4. **终极方案**：`git update-index --really-refresh <file>` 可以强制 Git 重新扫描文件，跳过 stat 缓存。

### 已落地规则

见 [.trae/rules/project_rules.md](./.trae/rules/project_rules.md) — 「文件修改规范」章节。

---

## 问题 #2 — GitHub 页面中文乱码（UTF-8 BOM 缺失）

**日期**：2026-05-18

**严重程度**：高

**状态**：已解决 ✅

### 问题描述

README.md 文件在本地 IDE（Trae Solo）和 raw.githubusercontent.com 中中文显示正常，但在 GitHub 仓库主页渲染时全部显示为乱码。问题页面：https://github.com/Shockley-HUANG/call_center_sip_system_trae_solo__deepseek_v4_pro

### 根因分析

| 编码格式 | GitHub 主页 | raw 文件 | 本地 IDE |
|----------|:-----------:|:--------:|:--------:|
| UTF-8 with BOM | ✅ 正常 | ✅ 正常 | ✅ 正常 |
| UTF-8 without BOM | ❌ 乱码 | ✅ 正常 | ✅ 正常 |

核心原因：**GitHub 主页的 Markdown 渲染引擎依赖 BOM（Byte Order Mark，文件头 `EF BB BF` 三字节）来自动检测文件编码**。如果文件缺少 BOM 前缀，GitHub 在无法确认编码的情况下会使用错误编码（如 latin-1 或 Windows-1252）解析中文字符，导致乱码。

`Write` 工具默认写入的是 UTF-8 without BOM，这是导致该问题的直接原因。

本地 IDE 和 raw.githubusercontent.com 之所以不受影响，是因为它们的编码检测算法更智能，可以在无 BOM 情况下通过内容分析自动判定 UTF-8。

### 尝试过的无效操作

| 操作 | 结果 |
|------|------|
| 多次 `Write` 重写 README.md | GitHub 主页依然乱码 |
| 多次 commit + push | GitHub CDN 缓存导致旧版本持续显示 |
| 等待 CDN 自动刷新 | GitHub 主页始终显示乱码（根本是编码问题） |
| 修改文件 mtime 强制刷新 | 无效 |
| `git update-index --really-refresh` + 重推 | 无效（因为文件编码本身没有 BOM） |

### 最终解决方案

**步骤 1**：删除旧的 README.md，用 `Write` 工具完整重写文件内容。

**步骤 2**：用 PowerShell 命令**强制补写 UTF-8 BOM**：

```powershell
$content = Get-Content README.md -Raw -Encoding UTF8
[System.IO.File]::WriteAllText("$PWD\README.md", $content, [System.Text.UTF8Encoding]::new($true))
```

关键点：`[System.Text.UTF8Encoding]::new($true)` 中的 `$true` 参数表示在文件头部写入 BOM（`EF BB BF`）。

**步骤 3**：验证 BOM 是否成功写入：

```powershell
$bytes = [System.IO.File]::ReadAllBytes("$PWD\README.md")
# 期望输出：239 187 191
Write-Output "BOM: $($bytes[0]) $($bytes[1]) $($bytes[2])"
```

**步骤 4**：强制刷新 Git 索引并提交推送：

```powershell
git update-index --really-refresh README.md
git add README.md
git commit -m "fix: README.md 添加UTF-8 BOM修复GitHub中文乱码"
git push origin main
```

最终 commit：`e596361`。通过 raw.githubusercontent.com 按 commit hash 验证（`/e596361/README.md`），中文已全部正常显示。

### 经验教训

1. **所有含中文内容的项目文件（README.md、ISSUES_LOG.md、docs/*.md 等），必须在写入后补写 UTF-8 BOM**。
2. `Write` 工具默认输出 UTF-8 without BOM，不能假设文件已包含 BOM。
3. 文件写入后必须验证 BOM 字节（`EF BB BF`）是否存在。
4. 不能用本地 IDE 或 raw.githubusercontent.com 的正常显示来判断 GitHub 主页是否正常 — 它们的编码检测机制不同。
5. GitHub 主页 CDN 缓存可能延长问题排查时间，验证时用 raw URL 按 commit hash 访问（如 `/e596361/README.md`）可绕过 CDN 缓存。

### 已落地规则

见 [.trae/rules/project_rules.md](./.trae/rules/project_rules.md) — 「文件编码规范」章节。

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
