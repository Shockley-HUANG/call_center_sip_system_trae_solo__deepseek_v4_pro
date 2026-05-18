# 项目规则 — 呼叫中心 SIP 系统

## 版本管控规范

1. 每完成一个功能节点，必须执行：`开发 → git add → git commit → git push origin main`
2. commit 信息格式：`feat:` / `fix:` / `docs:` / `refactor:` + 简要描述
3. 每次 commit 同时在 README.md 的「开发进展」表格追加一行（时间戳 + 版本 + 内容）
4. 远程仓库：https://github.com/Shockley-HUANG/call_center_sip_system_trae_solo__deepseek_v4_pro

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
4. 立即执行 `git add <file>` + `git diff --cached` 验证暂存区

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
