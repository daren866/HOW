# HOW - OpenHarmony/ArkUI Windows x64 兼容层

HOW 是一个把 **OpenHarmony / ArkUI 应用生态向 Windows x64 迁移** 的实验项目：
内置 **HOW Runtime**（轻量 ArkUI 兼容运行时），提供 HAP 安装器 + 应用列表 +
多窗口兼容层宿主，并附 GitHub Actions 自动 Windows 打包与 Release 发布。

![CI](https://github.com/daren866/HOW/actions/workflows/windows-build.yml/badge.svg)

## 功能

- 主窗口 **「HOW - x64转译arm模式」**
  - 中部列表展示每个已安装 HAP 应用的**应用名称**
  - 底部 **「安装hap」** 按钮：选择 `.hap` 文件 → 解析 `app.json5` → 弹出确认框 → 解压安装到 `%APPDATA%\HOW\apps\<bundleName>\`
- 单击列表项 → 创建 **「{App name} 兼容层」** 新窗口，启动运行时并渲染该应用
- 每个 HAP 包含 `ets/modules.abc`（Panda 容器）与 `pages/index.json`（ArkUI 声明式 UI 描述）

## HOW Runtime 架构

```
HAP 包 (zip)
 ├─ app.json5 ──────────► JSON5 宽松解析 → 应用名/包名/版本（安装确认）
 ├─ ets/modules.abc ────► Panda 容器解析（PACA 魔数 + 版本 + 字符串池）
 │                        └─ HOWRT 字节码段 → HOWVM 栈式虚拟机执行
 └─ pages/index.json ───► ArkUI 声明式组件树（column/row/text/button…）
                          ├─ measure/arrange 两遍式 Flex 布局引擎
                          ├─ GDI 双缓冲软渲染（圆角/颜色/字号/粗细）
                          └─ 命中测试 → 动作分发（inc/set/toast/vmcall）
```

- **abc.c**：真实 Panda（.abc）容器头解析 —— magic `PACA`、4 字节版本号、
  字符串池采样，并加载内嵌 HOWRT 代码段（格式参考 `arkcompiler_runtime_core/libpandafile/file.h`）
- **vm.c / abc.c(vm_run)**：HOWVM 微型栈式虚拟机（PUSHI/LOADG/STOREG/ADD/SUB/
  MUL/DIV/JMP/JMPZ/HALT），示例 HAP 的 `+1` 按钮就是真实执行字节码计数
- **ui_render.c**：两遍式 Flexbox 简化布局（measure → arrange）+ GDI 双缓冲绘制，
  支持 justifyContent(start/center/end/space-between)、align、padding/margin、
  圆角按钮、`{{state}}` 数据绑定与 Toast 悬浮层

## 构建

### GitHub Actions（推荐）

| 工作流 | 触发 | 产物 |
|---|---|---|
| `windows-build.yml` | push main / tag `v*` / 手动 | `HOW-windows-x64.zip`（MSVC x64）与 `HOW-windows-xp.zip`（MinGW PE32，子系统 5.01，兼容 WinXP）自动上传到 Releases |
| `ark-runtime.yml` | 手动 / 每周巡检 | 上游真实 `arkcompiler_ets_runtime` 与 `arkcompiler_runtime_core` 的 x64 构建尝试与日志 |

### 本机构建

```sh
# MSVC (Developer PowerShell)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# MinGW（支持 XP 目标）
make -f scripts/Makefile.mingw XP=1 CC=gcc HOW=HOW.exe
```

### 体验

1. 启动 `HOW.exe`
2. 点击 **安装hap** → 选择仓库内 `samples/MyFirstDemo.hap` → 确认安装
3. 双击/单击列表中的 **你好方舟** → 打开「你好方舟 兼容层」窗口
4. 点击 `+ 1` 按钮：HOWVM 执行 `.abc` 中的 `onPlus` 字节码并刷新渲染

## 上游仓库映射（Roadmap）

| OpenHarmony 仓库 | 职责 | HOW 现状 / 计划 |
|---|---|---|
| `arkui_ace_engine` | 声明式前端、组件树、布局、事件分发 | `ui_parse.c`/`ui_render.c` 实现声明式树 + 布局 + 命中分发（子集） |
| `arkcompiler_runtime_core` | 方舟运行时公共组件、Panda 文件格式 | `abc.c` 解析 PACA 容器与字节码执行（子集） |
| `arkcompiler_ets_runtime` | ArkTS .abc 解释执行/JIT/AOT VM | CI 工作流尝试上游 x64 构建 → 后续以进程内/独立进程方式接入 |
| `arkcompiler_ets_frontend` | es2panda：ArkTS → .abc | 计划接入 es2abc 工具链，直接编译 ArkTS 源码 |
| `graphic_graphic_2d` | Rosen 渲染服务、VSync、Skia 合成 | 现 GDI 软渲染；计划引入 Skia 后端 |
| `multimodalinput_input` | 多模输入事件采集 | Win32 消息（WM_LBUTTONUP 等）→ ui_click 分发 |
| `arkui-x` | ArkUI 跨平台扩展(Android/iOS) | Windows 平台层参考其 adapter 分层设计 |

## 目录结构

```
src/
 ├─ main.c        主窗口：标题、应用列表、安装hap按钮、安装确认流程
 ├─ compat.c      兼容层窗口：工具栏/页面/运行日志三区，加载 .abc + UI 模式
 ├─ store.c       HAP 安装/探测/清单（miniz 解压 + app.json5 解析）
 ├─ ui_parse.c    RtState 生命周期、日志、声明式组件树解析
 ├─ ui_render.c   Flex 布局 + GDI 双缓冲渲染 + 命中测试与动作执行
 ├─ abc.c         Panda .abc 容器解析 + HOWVM 虚拟机
 ├─ json.c        JSON5 宽松解析器
 └─ how.h/howcore.h
third_party/miniz/  公共领域 zip 读写库（解压 HAP）
samples/MyFirstDemo.hap  示例应用（含真实 PACA 头 .abc + HOWVM 字节码）
scripts/test_core.c      核心层 Linux 单测（18 项断言）
tools/make_sample_hap.py 样例 HAP 生成器
```

## 说明

- 本项目用于研究与学习 OpenHarmony/ArkUI 架构，非华为官方项目
- HOW Runtime 是极简兼容实现，不追求与 ArkTS 语义完全对齐；
  完整运行时的接入路线见上表 Roadmap
