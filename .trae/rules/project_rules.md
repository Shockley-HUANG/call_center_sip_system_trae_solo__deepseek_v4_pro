# 项目规则 — 呼叫中心 SIP 系统

## 版本管控规范

1. 每完成一个功能节点，必须执行：`开发 → git add → git commit → git push origin main`
2. commit 信息格式：`feat:` / `fix:` / `docs:` / `refactor:` + 简要描述
3. 每次 commit 同时在 README.md 的「开发进展」表格追加一行（时间戳 + 版本 + 内容）
4. 远程仓库：https://github.com/Shockley-HUANG/call_center_sip_system_trae_solo__deepseek_v4_pro

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

## 构建与运行

- 编译：`make`（生成 `build/sip_server`）
- 运行：`make run`
- 清理：`make clean` / `make distclean`
- 调试：`make debug` / `make gdb`

## 项目技术栈

- 语言：C11 + Lua 5.2/5.3/5.4
- 通信协议：SIP + RTP
- 并发模型：epoll
- 构建工具：GNU Make + GCC
- 开发工具：Trae Solo IDE + DeepSeek V4 PRO
