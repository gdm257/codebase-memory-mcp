# git url 原生索引 / 搜索

功能来源: [DeusData/codebase-memory-mcp#1143](https://github.com/DeusData/codebase-memory-mcp/issues/1143) — Index Git URL directly。

---

`index_repository` / `search_code` 直接接受 git url, 自动 clone + index, 缓存复用。从用户角度, git url 项目与 local path 项目使用体验完全一致。

## 现状（已实现）- PR #1143

### index_repository

- `索引 {"repo_path": "<git-url>"}` —— 首次 clone 到 cache, 校验通过后在同路径建图写 `<project_name>.db`, 失败/中断留下的 partial 目录会被清理并按有限次数重试。已缓存时执行 `git fetch` + checkout 到目标 ref；缓存有效但刷新失败时允许使用 stale cache。
- **已缓存的项目视为和 local path 完全一致**：建图完成后, 所有 `tool(project=…)` 都与 local path 同源。

### search_code

- `搜索 {"project": "<git-url>"}`（URL 格式或扁平格式均可） —— 有缓存时直接走图 rank（与 local path 一致）; 无缓存时 clone 后用 grep 搜索（无图谱增强）。
- 提示: `project` 支持两种等价格式：
  - URL: `https://github.com/o/r@main` 或 `git@github.com:o/r.git`
  - 扁平化: `github.com__o__r__main`（可直接从 `list_projects` 返回的 `name` 字段取得）

### project 参数格式

两种格式被**完全等价地**接受:

| 用户写法 | 内部归一化（传给 DB） |
|:---|:---|
| `github.com/o/r` | `github.com__o__r` |
| `github.com/o/r@main` | `github.com__o__r__main` |
| `https://github.com/o/r.git` | `github.com__o__r` |
| `git@github.com:o/r@main` | `github.com__o__r__main` |

- `/` → `__`（双下划线, 已是现有实现）。
- `@<ref>` 的 `/` 部分 `-` 转义为 `_`（避免 Windows 文件名冲突）。
- host 段 lowercased；段之间一律 `__` 分隔。

### list_projects 输出

`name` 字段统一返回**扁平化** project_name:

```json
{"name": "github.com__o__r__main", "kind": "git-url"}
{"name": "C-Users-demo-codebase-myproject", "kind": "local"}   // 老 local path 项目保持不变
```

用户看到的名字已经是可以直接塞进任何 tool 的 `project` 参数。

### 缺项目引导（强引导设计）

当 tool 接受的 project 不存在时, 若参数本身**看起来是 URL**（或扁平化后命中 `host__path` 模式），**专门引导** index 步骤而非通用报错:

```json
{
  "error": "project for URL has not been indexed",
  "url": "https://github.com/o/r@main",
  "project": "github.com__o__r__main",
  "hint": "project \"https://github.com/o/r@main\" has not been indexed — call "
          "{\"tool\":\"index_repository\",\"arguments\":{\"repo_path\":"
          "\"https://github.com/o/r@main\"}} first, then retry this tool."
}
```

目的: LLM 读到错误后**直接复制粘贴样板调用**即可完成"发现 → 索引"闭环, 无需二次询问。
若是**普通 project_name 缺失**（非 URL），沿用现有 `build_no_store_error` 返回已有 projects list。

## 认证

裸 `git clone`（调系统 git）。public 直通; 本机已配 credential helper / SSH key 的私有 repo 顺带能用。参数里不传 token。

## url 规范化

- Go 风格 `url@<ref>`（`<ref>` = branch / tag / commit; 不写 = remote 默认分支）。
- 等价类: 协议(https/ssh/git)、`.git` 后缀、trailing slash 去掉; host 小写; path 大小写保留; 端口保留; `@ref` 里的 `/` 转义。
- 同一 repo 不同 ref 各自独立 project + clone 目录，互不覆盖。

## 缓存

- clone 目录: `<CBM_CACHE_DIR>/git-cache/<host>/<path...>/[<@ref>/]`（`CBM_CACHE_DIR` 默认 `~/.cache/codebase-memory-mcp`）。
- **project DB 文件**: `<CBM_CACHE_DIR>/<project_name>.db`（扁平落盘，与 local path DB 同根目录）。
- host 做顶层 namespace（任意 git host 平等），path 保留 `/`（支持 GitLab subgroup 等多级）。
- **project_name**（过 `cbm_validate_project_name`，validator 现已 allow `@`）:
  - `/` 映射为 `__`（避免 `o/r` 与 `o-r` 撞同一个 DB）。
  - host 的 `:`（端口）等映射为 `_`。
  - ref 段整体拼在末尾：`host__path__ref`。
- **local path 项目完全兼容**: flat DB 命名 `C-Users-xxx.db` 等一行不动；tool 路径一致。

```
url                                  project_name              clone 目录
https://github.com/o/r.git           github.com__o__r          git-cache/github.com/o/r/
https://github.com/o/r.git@v1.2.0    github.com__o__r__v1_2_0  git-cache/github.com/o/r/v1.2.0/
git@github.com:o/r.git@main          github.com__o__r__main    git-cache/github.com/o/r/main/
https://gitlab.com/g/s/r.git         gitlab.com__g__s__r       git-cache/gitlab.com/g/s/r/
```

## 实现（C11）

- 新模块 `src/git/git_url.{c,h}`: `cbm_git_url_is_url` / `cbm_git_url_normalize` / `cbm_git_url_ensure_cloned`。调系统 git 走 `cbm_subprocess_run` + argv 数组（无 shell, url 免转义），复用 `src/foundation/subprocess.c`。
- **入口接线（均在 `src/mcp/mcp.c`**，**非** `cli.c`）:
  - `handle_index_repository`: `repo_path` 是 url → `ensure_cloned` → clone_dir 替换 repo_path + `name` 覆盖为 project_name。
  - `handle_search_code`: project 是 url → 仅 clone_dir 不存在时 clone（不 fetch，符合 Y） → clone_dir 当 root_path 走 grep（缺图则递归 grep fallback）; 已有 DB 走全套图 rank。
  - `normalize_project_arg`: url 走 `git_url_normalize` 的 project_name（`/`→`__` + `@ref` tail）, 保证 index 与 search/其他 tool 收敛到同一 DB。
- **新增 error helper**: `build_unindexed_url_error(args)` — 参数是 URL 但 DB 缺失时, 返回"直达 index_repository"的样板化 hint（与现有 `build_no_store_error` 解耦，保留老路径兜底）。
- **测试** `tests/test_git_url.c`: 纯函数 normalize 单测（`TEST` / `ASSERT_STR_EQ`, 零外部依赖）+ `build_unindexed_url_error` 文案单测。风格遵循 `.clang-format` / `.clang-tidy`。

## local path 与 git url 等价承诺

| 能力 | local path | git url |
|:---|:---|:---|
| `list_projects` 列出 | ✅ 完全一致格式 | ✅ |
| `tool(project=…)` 调用 | ✅ 使用扁平/URL 两格式 | ✅ 内部归一化后同路径 |
| 图查询 `search_graph` `trace_call_path` 等 | ✅ | ✅（建图完成后） |
| 自动 watch / detect_changes 自动触发 | ✅（local path 原生） | ❌ Out of scope |
| 缺项目报错 | 老 hint（projects list） | **新样板直达 index** |

## 约束

改动克制、集中: 新模块 + 两个入口最少接线 + 一个 error helper。

Out of scope:

- 每次 search 自动 pull / TTL 刷新（缓存优先）。
- 显式 token / 认证参数。
- 改用 git 协议库（保持调系统 `git`）。
- 多 ref 共享用 git worktree（每 ref 独立 clone 目录）。
- 大小写 case-escape（`!x`）。
- **nested .db layout**（项目 DB 一律 flat 落在 cache 根，不新建嵌套目录）。
- **改变现有 local path .db 命名**（老 project_name 中的 `-` 分隔符保留）。
