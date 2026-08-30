# Graft64：iOS Windows Runtime 项目需求文档与实现方案

- 文档状态：Draft v0.1
- 日期：2026-08-30
- 面向对象：Codex 开发代理、项目维护者
- 首发运行环境：Apple Silicon macOS 构建主机；iOS/iPadOS 17.4+ 真机；LiveContainer + StikDebug
- 核心上游：Wine upstream、FEX upstream
- 明确排除：Grape、Juice 及其代码、补丁、资源和构建脚本

---

## 1. 项目目标

Graft64 的目标是实现一个可长期维护的 iOS Windows runtime，使 Windows ARM64 与 x86-64 应用能够在 iPhone/iPad 上运行。

核心原则：

1. 以上游 Wine 负责 Win32/Win64 API、PE loader、ARM64EC、WoW64 和 Windows 运行时语义。
2. 以上游 FEX ARM64EC 模块负责 x86-64 到 ARM64 的 CPU 动态翻译。
3. 项目只维护尽可能薄的 iOS 平台适配层、UIKit 宿主、窗口驱动、JIT/内存适配、进程启动、打包和产品层。
4. 不维护大型 Wine fork，不维护 FEX 指令翻译器 fork。
5. 不以 Juice/Grape 为代码基础，也不复制其实现；只基于 Wine、FEX、Apple 官方接口和项目自身验证结果开发。
6. 所有关键可行性结论必须来自可复现的真机日志，不能以“理论可行”代替验证。

项目定位：

> Upstream-first Windows runtime for iOS.

---

## 2. 架构基线

### 2.1 目标执行链

```text
Graft64 UIKit Host
        |
        | launch / lifecycle / files / input / display
        v
Wine iOS platform glue + wineios.drv
        |
        v
Wine ARM64X / ARM64EC
        |
        +--------------------+
        |                    |
Windows ARM64 PE       Windows AMD64 PE
        |                    |
 native ARM64          ARM64EC emulator ABI
                             |
                     libarm64ecfex.dll
                             |
                           FEXCore
                             |
                       ARM64 JIT code
                             |
                         iOS ARM64 CPU
```

### 2.2 组件职责

| 组件 | 责任 | 维护方 |
|---|---|---|
| Wine | Win32/Win64 API、PE、ARM64EC、WoW64、wineserver、prefix | Wine upstream |
| FEX | x86/x64 解码、IR、ARM64 JIT、ARM64EC 模拟接口 | FEX upstream |
| Graft Platform Layer | iOS JIT、路径、helper 启动、IPC、日志、生命周期适配 | 本项目 |
| wineios.drv | GDI surface、窗口、输入、剪贴板、显示协议 | 本项目 |
| GraftHost | UIKit/SwiftUI UI、导入、运行、日志、配置 | 本项目 |
| Runtime Builder | 固定上游版本、构建、patch、打包、校验 | 本项目 |

### 2.3 不额外设计 CPU 后端抽象

第一阶段只支持 FEX ARM64EC。不要自行设计一套 FEX/Box64 通用 CPU 后端接口。

Wine 已经提供外部 AMD64 模拟库接口。未来只有在 Box64 upstream 正式实现 ARM64EC 模块后，才通过 Wine 现有接口增加 Box64；当前不自行实现 Box64 ARM64EC bridge。

---

## 3. 项目范围

### 3.1 首期范围

首期必须按顺序完成：

1. iOS Host Probe：验证 JIT、16K 页面、信号恢复、动态库、helper 进程、IPC。
2. 可复现的 Wine/FEX upstream 构建系统。
3. iOS 上运行 ARM64 Windows 控制台程序。
4. iOS 上通过 FEX ARM64EC 运行 x86-64 Windows 控制台程序。
5. 最小单窗口 GDI/Framebuffer 驱动。
6. 基础输入、软键盘、文件导入、日志和 prefix 管理。

### 3.2 非首期范围

以下内容禁止在 G0-G3 阶段投入：

- Direct3D、DXVK、VKD3D、MoltenVK。
- 完整桌面 shell、任务栏、资源管理器。
- 32 位 x86 应用支持。
- Windows 服务完整兼容。
- 多窗口复杂合成。
- 游戏优化和兼容性配置数据库。
- App Store 分发。
- 同时维护 FEX 与 Box64 两个后端。
- 为了快速跑通而大范围复制 Juice/Grape patch。
- 通过大量 stub 假装 Wine 子系统已可用。

---

## 4. 目标环境

### 4.1 构建环境

#### macOS 构建主机

- Apple Silicon Mac。
- 当前可用的 Xcode 和 iPhoneOS SDK。
- CMake、Ninja、Python 3、Git。
- 用于构建 GraftHost、Graft Platform Layer、iOS helper、IPA。
- 所有脚本必须通过 `xcrun` 获取 SDK 和工具路径，不允许写死 Xcode 路径。

#### ARM64 Linux 构建环境

- Ubuntu/Debian ARM64 环境。
- 推荐在 Apple Silicon 上使用原生 `linux/arm64` 容器或 ARM64 虚拟机。
- 用于构建 Wine ARM64X/ARM64EC 和 FEX ARM64EC。
- 不要求在 x86_64 主机上通过 QEMU 完成正式构建。
- 构建环境必须容器化并锁定依赖版本。

### 4.2 运行环境

初始目标：

```text
iOS/iPadOS 17.4+
LiveContainer
StikDebug
get-task-allow
JIT enabled
arm64 device
```

后续可增加：

- TrollStore。
- 越狱环境。
- 独立 sideload 宿主。

不同宿主必须通过 adapter 隔离，不能把 LiveContainer 路径写死到 Wine/FEX 核心补丁中。

---

## 5. 可维护性约束

### 5.1 Upstream-first 规则

依赖采用固定版本或固定 commit：

```text
third_party/manifest/deps.lock
```

必须记录：

- repository
- ref/tag
- commit SHA
- source archive SHA-256
- build toolchain version
- applied patch series
- output artifact SHA-256

禁止直接在 `third_party/wine` 或 `third_party/fex` 中提交未记录修改。所有修改必须保存为：

```text
patches/wine/*.patch
patches/fex/*.patch
```

并通过脚本重放。

### 5.2 Patch 预算

软性控制线：

- Wine 私有 patch：不超过 30 个。
- FEX 私有 patch：不超过 10 个。
- FEX 私有修改原则上不进入 decoder、IR optimizer、ARM64 code generator、SSE/AVX 核心。
- 超过 10,000 行 Wine 差异或 2,000 行 FEX 差异时，必须新增 ADR 解释继续维护的理由。
- 每个无法 upstream 的 patch 必须有 `PATCH-METADATA.yaml`，说明用途、风险、上游 issue/MR 和移除条件。

### 5.3 禁止事项

- 不维护长期滚动的 `graft-wine` 大型分支。
- 不维护长期滚动的 `graft-fex` 大型分支。
- 不提交编译后的大型 Wine/FEX runtime 到 Git 历史。
- 不在业务代码中散布运行环境判断；统一放入 `platform/ios` 和 host adapter。
- 不把失败的系统调用吞掉并返回成功。
- 不声称真机通过，除非保存了设备报告和日志。
- 不把 pairing file、证书、团队 ID、开发者账号、设备 UDID 写入仓库或日志。

---

## 6. 推荐仓库结构

```text
graft64/
├── AGENTS.md
├── README.md
├── LICENSE
├── THIRD_PARTY_NOTICES.md
├── docs/
│   ├── REQUIREMENTS.md
│   ├── ARCHITECTURE.md
│   ├── FEASIBILITY.md
│   ├── BUILDING.md
│   ├── DEVICE_TESTING.md
│   ├── UPSTREAM_GAPS.md
│   ├── BOX64_WATCH.md
│   └── adr/
│       ├── 0001-upstream-first.md
│       ├── 0002-livecontainer-first.md
│       ├── 0003-process-model.md
│       └── 0004-page-model.md
├── app/
│   └── GraftHost/
├── platform/
│   ├── include/graft/
│   ├── darwin/
│   └── tests/
├── probes/
│   ├── jit/
│   ├── vm/
│   ├── signals/
│   ├── helper/
│   ├── ipc/
│   ├── dylib/
│   └── lifecycle/
├── runtime/
│   ├── wine/
│   ├── fex/
│   ├── prefix/
│   └── packaging/
├── driver/
│   └── wineios.drv/
├── samples/
│   ├── windows-arm64/
│   └── windows-amd64/
├── third_party/
│   └── manifest/
├── patches/
│   ├── wine/
│   └── fex/
├── scripts/
│   ├── bootstrap-macos.sh
│   ├── build-probes.sh
│   ├── package-ipa.sh
│   ├── fetch-upstream.sh
│   ├── apply-patches.sh
│   ├── build-runtime-linux-arm64.sh
│   ├── verify-artifacts.sh
│   └── patch-stats.sh
├── containers/
│   └── runtime-builder/
├── tests/
│   ├── host/
│   ├── device/
│   └── compatibility/
└── out/
```

`out/` 和下载后的大型源码、runtime 必须加入 `.gitignore`。

---

## 7. 实现阶段与验收关卡

# G0：iOS 宿主可行性验证

目标：在不引入 Wine/FEX 的情况下，验证 iOS/LiveContainer 环境是否具备承载 Wine/FEX 的基础能力。

必须验证：

1. App 在 LiveContainer 中正常启动。
2. 正确解析 guest app bundle、真实 executable、Documents、Library、tmp 路径。
3. 获取真实宿主页大小。
4. 使用 `MAP_JIT` 分配 JIT 区域。
5. 写入并执行最小 ARM64 代码。
6. RW/RX 或 JIT write-protect 切换。
7. 多线程 JIT。
8. instruction cache flush。
9. `sigaltstack` + `sigaction`。
10. 捕获 SIGSEGV/SIGBUS，并修改 `ucontext_t` 恢复执行。
11. `dlopen` 加载包内签名 dylib。
12. Unix domain socket 或 socketpair。
13. 文件映射共享内存。
14. 启动预签名 bundled helper。
15. helper 与 host 双向 IPC。
16. App background/foreground 后重新执行 JIT。
17. 导出 JSON 测试报告。

### G0 强制止损条件

以下任一结果为红色阻断：

- 在 StikDebug 已确认启用 JIT 后，最小 ARM64 JIT 仍无法执行。
- 无法使用信号和 `ucontext_t` 从受控 fault 恢复。
- 无法启动 helper，且没有可验证的 LiveContainer 多进程替代机制。
- helper 无法建立可靠 IPC。
- JIT 页面在前后台切换后不可恢复。
- 4K 逻辑页需求必须通过修改 FEX 每条内存访问才能实现。

遇到红色阻断时，不进入 Wine 移植；输出事实日志、根因假设和替代架构评估。

# G1：上游构建与 Linux ARM64 基线

目标：先在官方支持度更高的 ARM64 Linux 环境验证 Wine ARM64EC + FEX。

任务：

1. 固定 Wine stable tag 和 FEX release tag。
2. 构建 ARM64EC 工具链。
3. 构建 Wine ARM64X/ARM64EC。
4. 构建 FEX ARM64EC 模块。
5. 编译 ARM64、AMD64 Windows 测试程序。
6. 在 ARM64 Linux 上运行：
   - ARM64 hello。
   - AMD64 hello，经 FEX。
   - 线程/TLS。
   - VirtualAlloc/VirtualProtect。
   - 异常。
   - DLL 加载。
7. 打包可复现 runtime artifact 和 manifest。

注意：若 FEX 文档建议某个个人 Wine fork，不得静默替换 upstream Wine。必须先以 Wine upstream 验证；存在缺口时写入 `docs/UPSTREAM_GAPS.md`，提出最小 patch 或上游提交方案。

# G2：Wine ARM64 iOS 控制台路径

目标：不接入 FEX、不实现 GUI，先运行 ARM64 Windows 控制台程序。

范围：

- Wine loader。
- wineserver。
- 最小 prefix。
- 文件系统。
- 线程、TLS、同步。
- signals/exceptions。
- helper 启动。
- Unix socket/共享内存。
- stdout/stderr 重定向到 GraftHost 日志。

验收：

- `hello-arm64.exe` 成功退出，退出码正确。
- 文件读写测试通过。
- 线程/TLS 测试通过。
- 受控异常测试通过。
- 子进程测试结果明确。
- wineserver 生命周期可控。
- App 重启后 prefix 保留。
- Wine 私有 patch 有完整统计。

# G3：FEX ARM64EC x64 路径

目标：通过 Wine 外部 AMD64 模拟接口加载 `libarm64ecfex.dll`，运行 AMD64 Windows 控制台程序。

任务：

- 将 FEX ARM64EC DLL 打包进 runtime。
- 配置 Wine AMD64 emulator registry。
- 将 FEX JIT 内存请求接入已验证的 iOS JIT 路径。
- 验证代码缓存、异常、线程、本地 TLS、自修改代码失效。
- 输出 FEX 配置和诊断日志。

验收：

- `hello-amd64.exe` 成功运行。
- 整数、浮点、SSE 基础测试通过。
- 多线程测试通过。
- VirtualAlloc/VirtualProtect 测试通过。
- DLL call boundary 测试通过。
- 自修改代码测试结果明确。
- 不依赖 x86 Linux rootfs。
- 没有对 FEX decoder/JIT core 做未记录修改。

# G4：最小 UIKit/GDI

目标：支持一个 Windows 窗口和 CPU framebuffer。

首版约束：

- 单顶层窗口。
- BGRA8888 surface。
- 仅 GDI/DIB。
- host 通过 Metal 或 CoreAnimation 显示。
- 鼠标、触摸、基础键盘、文本输入。
- 不做 Direct3D。
- 不做复杂窗口合成。

建议控制协议：

```c
struct graft_msg_header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payload_size;
    uint64_t request_id;
};
```

首期消息：

```text
HELLO
CREATE_SURFACE
DESTROY_SURFACE
RESIZE_SURFACE
PRESENT_DAMAGE
POINTER_EVENT
KEY_EVENT
TEXT_EVENT
SET_TITLE
CLIPBOARD_GET
CLIPBOARD_SET
FILE_OPEN_RESULT
SHUTDOWN
```

验收：

- 能显示简单 Win32 GDI sample。
- 屏幕重绘稳定。
- 触摸可映射为鼠标。
- 软键盘可输入 ASCII 与 UTF-8/UTF-16 基础文本。
- 前后台恢复后窗口继续工作。
- host 与 driver 协议有版本协商。

# G5：基础产品化

- 应用导入与 PE 架构识别。
- prefix 创建、复制、删除、快照。
- 启动参数和环境变量。
- 字体安装。
- 日志导出。
- crash report 汇总。
- 基础音频。
- 多窗口。
- 安装器验证。

# G6：图形与游戏

G6 必须作为独立项目评审，不在前述阶段顺带实现。

候选路线：

- WineD3D 到可用 OpenGL/Metal 层。
- DXVK/VKD3D + Vulkan 到 Metal。
- 自研有限 D3D 到 Metal 映射。

进入 G6 前必须先有稳定的 G3/G4 兼容性基线。

---

## 8. 首个可直接交给 Codex 的任务

# GRAFT-0001：创建 Graft64 仓库与 iOS Host Probe

## 8.1 任务目标

创建一个可编译、可打包、可导入 LiveContainer 的 iOS 应用 `GraftHost`，实现 G0 所需的宿主能力探针。

本任务不引入 Wine，不引入 FEX，不实现 Windows runtime。

## 8.2 必须交付

### A. 仓库骨架

按第 6 节创建目录和基础文档，当前无内容的后续目录可保留 `.gitkeep`。

创建：

- `AGENTS.md`
- `README.md`
- `docs/REQUIREMENTS.md`
- `docs/ARCHITECTURE.md`
- `docs/FEASIBILITY.md`
- `docs/DEVICE_TESTING.md`
- `docs/adr/0001-upstream-first.md`
- `docs/adr/0002-livecontainer-first.md`
- `docs/adr/0003-process-model.md`
- `docs/adr/0004-page-model.md`

### B. GraftHost iOS App

要求：

- SwiftUI 或 UIKit 均可，优先使用 SwiftUI + C bridge。
- deployment target：iOS 17.4。
- device architecture：arm64。
- UI 至少包含：
  - 环境信息。
  - JIT 状态说明。
  - Run All。
  - 单项运行。
  - 每项 PASS/FAIL/SKIP/BLOCKED。
  - 详细日志。
  - 导出 JSON。
  - 复制诊断摘要。
- App 不依赖网络。
- App 不读取或存储 pairing file。
- 不内置 StikDebug 代码；由用户在外部启用 JIT。

### C. Project-owned C API

创建稳定的 C ABI，Swift 只通过 C API 调用底层 probe。

建议文件：

```text
platform/include/graft/graft_probe.h
platform/include/graft/graft_jit.h
platform/include/graft/graft_process.h
platform/include/graft/graft_log.h
```

最小接口：

```c
typedef enum graft_probe_status {
    GRAFT_PROBE_PASS = 0,
    GRAFT_PROBE_FAIL = 1,
    GRAFT_PROBE_SKIP = 2,
    GRAFT_PROBE_BLOCKED = 3,
} graft_probe_status;

typedef struct graft_probe_result {
    const char *name;
    graft_probe_status status;
    graft_probe_reason_code reason_code;
    graft_error_code graft_error;
    int os_error;
    uint64_t duration_ns;
    const char *summary;
    const char *details_json;
} graft_probe_result;

typedef void (*graft_probe_callback)(const graft_probe_result *result,
                                     void *context);

int graft_run_probe(const char *name,
                    graft_probe_callback callback,
                    void *context);

int graft_run_all_probes(graft_probe_callback callback,
                         void *context);
```

字符串生命周期必须明确；建议 callback 返回前有效，Swift 立即复制。

JIT API：

```c
typedef struct graft_jit_region {
    void *base;
    size_t size;
    int backend;
} graft_jit_region;

int graft_jit_alloc(size_t size, graft_jit_region *out_region);
int graft_jit_begin_write(graft_jit_region *region);
int graft_jit_end_write(graft_jit_region *region);
int graft_jit_invalidate_icache(graft_jit_region *region,
                                size_t offset,
                                size_t size);
void graft_jit_free(graft_jit_region *region);
```

实现必须隐藏 `MAP_JIT`、write-protect 和 cache flush 细节。

### D. Probe 集合

必须实现以下 probe：

#### `runtime_paths`

记录：

- `_NSGetExecutablePath`
- `NSBundle.main.bundleURL`
- `Bundle.main.executableURL`
- `argv[0]`
- current working directory
- home
- Documents
- Library
- tmp
- binary UUID/版本
- 是否检测到 LiveContainer 环境；检测必须使用可解释的路径/环境证据，不能只依赖一个未经文档化的 magic 值

#### `page_model`

记录：

- `getpagesize()`
- `sysconf(_SC_PAGESIZE)`
- allocation granularity
- 对 64K 区域按 4K 偏移调用 `mprotect` 的实际结果
- 同一 host page 内不同逻辑权限的可行性
- 不应破坏进程；所有 fault 必须可恢复或在子 helper 中执行

#### `jit_basic`

- `mmap(... MAP_JIT ...)`
- 写入最小 ARM64 函数，返回固定值 42
- flush icache
- 执行并验证返回值
- 若 JIT 未启用，返回 BLOCKED，不要崩溃

#### `jit_write_protect`

- 验证 write-enable / execute-enable 切换
- 验证代码更新后重新执行
- 记录使用的系统能力

#### `jit_multithread`

- 至少 4 个线程
- 每线程重复执行 JIT 函数
- 验证 write-protect API 的线程语义
- 设置合理次数，避免长时间发热

#### `signal_resume`

- `sigaltstack`
- `sigaction` with `SA_SIGINFO`
- 在受控 helper 或隔离代码段中制造 fault
- 读取 fault address 与寄存器上下文
- 修改 PC 跳转到恢复点
- 验证进程继续执行
- 不允许通过忽略信号伪造 PASS

#### `dlopen_bundle`

- 打包一个签名测试 dylib
- `dlopen`、`dlsym`、调用并验证返回值
- 记录实际路径和错误信息

#### `unix_socket`

- socketpair 或 AF_UNIX
- 双向消息
- 超时处理
- 关闭处理

#### `shared_mapping`

- 使用文件映射建立共享区域
- host/helper 双向读写并校验
- 记录 mmap flags 和失败 errno

#### `helper_spawn`

- 打包一个最小 arm64 Mach-O helper
- 使用优先级如下：
  1. 项目定义的 host adapter
  2. `posix_spawn`
  3. LiveContainer 已有且可公开调用的机制
- helper 返回 PID、page size 和随机 nonce
- host 验证 exit code
- 禁止依赖 shell

#### `helper_ipc`

- host 启动 helper
- 通过 Unix socket 通信
- 至少完成 HELLO、PING、SHUTDOWN
- 所有消息有 magic、version、length
- 使用超时避免永久挂起

#### `lifecycle_jit`

- 首次 JIT 通过后记录状态
- App 进入后台再回到前台
- 用户手动再次运行
- 报告 JIT 是否仍可用
- 自动测试无法可靠触发生命周期时标为需要手工步骤，不得伪造自动通过

### E. JSON 报告

输出位置：

```text
Documents/Graft64/Reports/<UTC timestamp>-<device>.json
```

格式至少包含：

```json
{
  "schema_version": 1,
  "app_version": "...",
  "build_commit": "...",
  "timestamp_utc": "...",
  "device": {
    "model": "...",
    "system_name": "...",
    "system_version": "...",
    "machine": "...",
    "page_size": 0
  },
  "environment": {
    "livecontainer_detected": false,
    "jit_expected": true
  },
  "probes": []
}
```

不得记录：

- UDID
- pairing record
- developer account
- certificate private data
- user文件内容

### F. Helper 与协议

创建：

```text
probes/helper/GraftProbeHelper
platform/include/graft/graft_ipc_protocol.h
```

协议要求：

```c
#define GRAFT_IPC_MAGIC 0x47524654u
#define GRAFT_IPC_VERSION 1u
```

header 必须包含：

- magic
- version
- type
- payload length
- request ID

解析器必须检查整数溢出和最大 payload；首版最大 payload 64 KiB。

### G. 构建脚本

必须提供：

```text
scripts/bootstrap-macos.sh
scripts/build-probes.sh
scripts/package-ipa.sh
scripts/verify-package.sh
```

要求：

- `set -euo pipefail`
- 检查必需工具
- 不使用硬编码 Xcode 路径
- 输出全部位于 `out/`
- `package-ipa.sh` 生成 `out/GraftHost.ipa`
- `verify-package.sh` 检查：
  - Payload 结构
  - 主 executable
  - helper
  - 测试 dylib
  - executable bit
  - Mach-O architecture
  - Info.plist
  - entitlements 报告
  - SHA-256 manifest

### H. 测试

只要求高价值测试：

- IPC header encode/decode。
- JSON schema serialization。
- probe registry。
- 超时和错误传播。
- JIT API 在 macOS 上的 compile/test；真机语义由 device test 验证。
- 不建设大型 mock 框架。

### I. CI

CI 至少完成：

- 格式检查。
- C/C++ host 单测。
- Swift compile 或 iOS simulator compile。
- shellcheck。
- package structure 静态验证。
- 不要求 CI 模拟 iOS 真机 JIT。

## 8.3 验收标准

代码层：

- 项目可从干净 clone 构建。
- 所有脚本无交互运行。
- 主工程无警告，项目自有 C/C++ 代码启用严格 warning。
- 无硬编码用户目录、Team ID、设备 ID。
- 失败包含 API、errno、路径和步骤。
- 不存在永远返回 PASS 的占位实现。

设备层：

- IPA 可被 LiveContainer 导入。
- GraftHost 能启动。
- 能运行单个和全部 probe。
- 报告可导出。
- `jit_basic` 在启用 StikDebug JIT 后通过，或给出完整失败日志。
- helper 测试必须得到明确 PASS/FAIL，不能永久等待。
- 前后台测试有人工验证步骤。

文档层：

- `FEASIBILITY.md` 给出 Green/Yellow/Red 结论。
- 每个失败 probe 有根因候选和下一步实验。
- 明确是否允许进入 G1。
- 不根据代码审查猜测真机结果。

## 8.4 Codex 工作约束

1. 先阅读已有仓库，不覆盖用户已有修改。
2. 小步提交，功能与文档同步。
3. 不使用 Juice/Grape 代码或 patch。
4. 不引入 Wine/FEX，GRAFT-0001 仅做 host probe。
5. 不为了通过测试降低测试强度。
6. 无法在当前环境真机测试时：
   - 完成可编译代码；
   - 给出精确安装步骤；
   - 将真机项标为 UNVERIFIED；
   - 不声称通过。
7. 任何 private API 使用必须先写 ADR；默认禁止。
8. 优先使用公开 Apple API。
9. 所有外部依赖必须记录许可证和固定版本。
10. 不将生成的 IPA、证书、profile、大型二进制提交到 Git。
11. 遇到架构阻断时停止扩功能，先更新 `FEASIBILITY.md`。
12. 最终回复必须包含：
    - 修改文件摘要；
    - 构建命令；
    - 测试结果；
    - 未验证项；
    - 已知风险；
    - 下一阶段是否满足进入条件。

---

## 9. GRAFT-0002：上游构建基线

只有 GRAFT-0001 的 G0 结论允许继续时，才能执行。

目标：

- 在 ARM64 Linux builder 中构建固定版本的 Wine ARM64X 与 FEX ARM64EC。
- 不做 iOS 移植。

### 交付

```text
containers/runtime-builder/Dockerfile
third_party/manifest/deps.lock
scripts/fetch-upstream.sh
scripts/build-runtime-linux-arm64.sh
scripts/run-linux-baseline.sh
samples/windows-arm64/*
samples/windows-amd64/*
docs/UPSTREAM_GAPS.md
out/runtime-linux-arm64/*
```

### 构建策略

Wine：

- 首选 Wine stable upstream。
- 配置 ARM64EC + AArch64/ARM64X。
- 使用 clang/LLVM MinGW。
- 初始禁用不必要测试和图形依赖，但保留可扩展配置。
- 记录完整 configure 输出。

FEX：

- 使用 upstream release。
- 只构建 Wine 所需 ARM64EC 模块。
- 禁用 Linux rootfs/thunk 等与 ARM64EC module 无关的组件。
- 不修改 decoder/JIT core。

### 验收

- 构建过程可重复。
- artifact manifest 完整。
- ARM64 hello 通过。
- AMD64 hello 经 FEX 通过。
- 上游缺口被记录。
- 无个人 Wine fork 被静默引入。

---

## 10. GRAFT-0003：Wine ARM64 iOS Console Spike

目标：把 upstream Wine 的最小 ARM64 控制台路径移植到 iOS。

本任务是 spike，不承诺 GUI 和 x64。

核心实现：

1. 将 Wine Unix-side target 适配到 iPhoneOS arm64。
2. 构建 Wine host tools 与 target artifacts。
3. 设计预签名 loader/wineserver helper。
4. 实现 GraftHost 到 Wine helper 的启动和日志通道。
5. 复用 G0 已验证的 signal、IPC、shared mapping。
6. 创建最小 prefix。
7. 运行 ARM64 Windows sample。

修改边界：

- 允许修改 build/configure、Unix host glue、路径、进程启动、VM/JIT adapter。
- 禁止重写 Wine PE loader、ARM64EC、ntdll 通用逻辑。
- 每个 Wine patch 独立、可重放、带说明。

验收以 `hello-arm64.exe` 为唯一硬目标；不要在本任务实现 UI。

---

## 11. GRAFT-0004：FEX ARM64EC iOS Spike

前置条件：

- G0 Green。
- G2 ARM64 Wine sample 通过。
- Linux ARM64 FEX baseline 通过。

目标：

- 在 iOS Wine runtime 中加载上游 `libarm64ecfex.dll`。
- 运行 `hello-amd64.exe`。

实现顺序：

1. 验证 DLL 被 Wine 加载。
2. 验证 ARM64EC emulator initialization。
3. 验证 JIT code cache。
4. 验证 x64 entry point。
5. 验证 native Wine call boundary。
6. 再验证线程、TLS、异常和 VirtualProtect。

止损：

- 如需大范围修改 FEX JIT core，先暂停并提交 ADR。
- 如 16K/4K 语义只能通过每次内存访问的软件检查实现，重新评估兼容性目标。
- 不在本任务引入 Box64。

---

## 12. GRAFT-0005：最小 wineios.drv

目标：显示单窗口 GDI 应用。

约束：

- 参考 Wine upstream 的 user driver 与 `winemac.drv` 结构。
- 不复制 Juice/Grape driver。
- host-driver 协议独立、版本化。
- 初版只支持单 surface。
- surface 传输优先复用已验证共享内存，不提前依赖复杂跨进程 IOSurface 权限。
- GraftHost 使用 Metal 仅做纹理上传和显示，不实现 D3D。

验收应用：

- 自有 Win32 GDI sample。
- 一个简单上游 Wine GUI sample。
- 基础文本输入。
- pointer click。
- resize。
- background/foreground。

---

## 13. Box64 观察策略

当前不实现 Box64 路线，仅保留季度评估文档。

只有同时满足以下条件才重新评估：

1. Box64 upstream 正式提供 Wine AMD64 emulator/ARM64EC 模块。
2. 不需要 x86 Linux rootfs。
3. ABI transition、异常和线程由 upstream 维护。
4. 能通过 Wine 的现有 external emulator interface 接入。
5. 项目不需要维护 Box64 core 私有 fork。
6. 4K/16K 页面行为有可验证优势。

不因单个 benchmark 或理论性能推翻当前 FEX ARM64EC 架构。

---

## 14. 质量与安全要求

- v0.x 只允许运行项目自带可信测试程序。
- 不把 Windows workload 当作安全沙箱。
- 默认不开放网络。
- 文件导入必须由用户主动选择。
- prefix 与宿主文件系统路径必须隔离。
- 日志不得包含用户文件内容。
- 所有 IPC payload 有长度上限。
- 所有 helper 启动有超时和退出清理。
- 所有共享内存有生命周期和权限说明。
- 崩溃报告必须包含 build commit、probe、errno、signal、fault address，但不得包含隐私数据。

---

## 15. 最终决策标准

继续投入完整 runtime 的最低条件：

```text
G0:
JIT                    PASS
signal/ucontext         PASS
helper process          PASS 或有等价可维护方案
IPC/shared mapping      PASS
lifecycle JIT           PASS
page model              至少能支持目标测试集

G1:
Wine ARM64X build       PASS
FEX ARM64EC Linux       PASS

G2:
ARM64 Windows hello     PASS

G3:
AMD64 Windows hello     PASS
```

在 `hello-amd64.exe` 真机通过之前，不投入完整 GUI、安装器和 3D 图形。

---

## 16. 给 Codex 的简版执行指令

```text
请按 docs/REQUIREMENTS.md 执行 GRAFT-0001。

目标是创建 Graft64 iOS Host Probe，不要引入 Wine、FEX、Juice 或 Grape。

优先完成：
1. 仓库和文档骨架；
2. GraftHost iOS 17.4+ arm64 App；
3. C ABI probe framework；
4. JIT、page model、signal resume、dlopen、helper spawn、IPC、shared mapping probes；
5. JSON 报告；
6. 可复现 IPA 打包和静态校验；
7. 高价值 host 测试；
8. FEASIBILITY.md。

无法真机验证的项目必须标为 UNVERIFIED，不得声称成功。
helper、JIT、signal 任一核心能力失败时，不继续实现 Wine/FEX。
禁止复制或引用 Juice/Grape 的代码、补丁和资源。
最终输出修改文件、构建命令、测试结果、未验证项、阻断风险和是否满足进入 GRAFT-0002 的结论。
```
