# YCode Mobile Debugger (MixDeviceDebugger)：Visual Studio 插件 + LLDB Host 架构与协议设计方案

## 1. 项目目标

YCode Mobile Debugger 的目标是在 Windows 上为 Visual Studio 提供 iOS / Android 原生远程调试能力，覆盖安装、启动、Attach、断点、线程、调用栈、变量、表达式求值、内存查看、内存断点、符号管理以及性能分析联动。

## 0. 当前实施路线更新（2026-04-30）

Visual Studio 插件当前继续沿用已经落地的旧 MIEngine 路线，不在本阶段切换到
自研 AD7 Debug Engine；但实现顺序调整为先把 `llvm-project/dtx` 的 runtime
与 codegen 打底，方便 DebugHost 与后续 IDE 接入一气呵成。VSIX 在 DebugHost
可用前仍走旧桥接路径：

```text
Visual Studio
  ↓
YCode / Mix VSIX
  ↓
MIEngine + lldb-mi
  ↓
DebugServerForwarder (loopback TCP)
  ↓
mixdevice FFI
  ↓
iOS debugserver / Android lldb-server
```

当前阶段的工程目标：

```text
1. 保留 MIEngine 承担 VS 调试窗口、断点、线程、调用栈、变量和表达式集成。
2. VSIX 继续负责设备列表、启动 / Attach UI、符号路径传递、日志和屏幕工具窗口。
3. DebugServerForwarder 继续把 MIEngine/lldb-mi 的本地 TCP 连接转发到设备调试服务。
4. 优先补齐项目系统 F5 启动、Attach-to-Process 下拉双击 attach、符号路径自动发现和连接诊断。
5. DTX / DebugHost / YDP 先作为底层基础设施推进；VSIX 切换到它们之前仍保留 MIEngine 旧路径。
```

因此，当前执行策略是：先实现 DTX runtime + schema + codegen，再实现
DebugHost 服务，最后让 VS 插件从 MIEngine 桥接逐步迁移。当前可交付 VSIX 仍以
MIEngine 方案为准。

> 2026-04-30 DebugHost 实现更新：`YCode.DebugHost.exe` 的实现入口已落到
> `lldb/tools/lldb-mixdev`，作为 LLDB 工具树里的独立 host executable 构建。
> 该目标依赖 `dtxRuntime` 与 `dtx-codegen`，构建时生成 `DebugHostCore.td`
> 对应的 C++ server dispatch glue；当前 MVP 已注册 `DebugHostCore.td` 中的全部
> service processor：`Lifecycle`、`Session`、`Execution`、`Breakpoints`、
> `Threads`、`Expressions`、`Memory`、`Device`。其中 `Lifecycle`、`Session`、
> `Execution`、`Breakpoints`、`Threads`、`Expressions`、`Memory` 已接入
> `liblldb` SB API 后端，`Device.list` 已接到 `mix_device`；移动设备
> debugserver / lldb-server 的 mix 连接已按
> `mix_device_create_lldb_connection` 路线接入 LLDB gdb-remote，`Device.prepareDebug`
> 会调用 `mix_device_prepare_tools`。设备能力通过 IrisBuild 的
> `mix_device.h` / `mix_device.dll.lib` 接入；若本机没有 mix_device SDK，目标仍可
> 编译，但设备枚举会返回不可用状态。
> 当前验证构建目录为 `F:\llvm-project\.build\x64`，通过 VS2022 `vcvars64.bat`
> 初始化 MSVC 环境后执行 `ninja -C F:\llvm-project\.build\x64 ycode-debughost`。

核心定位不是简单包装 LLDB，而是提供一套面向移动开发的完整调试链路：

```text
Visual Studio
  ↓
YCode VS Extension / Debug Engine
  ↓
DTX Protocol  (规划见 llvm-project/dtx/PLAN.md)
  ↓
YCode Debug Host
  ↓
LLDB / Device Bridge / Symbol Manager
  ↓
iOS debugserver / Android lldb-server
```

IDE ↔ DebugHost 的所有交互都使用 DTX 协议承载，**不**自定义新的线协议。
DTX 是 IDE ↔ DebugHost 之间的统一 RPC：DebugHost 充当 DTX server，VS 插件
（含 AD7 Debug Engine）以及未来的 VS Code / Rider / Qt IDE 插件都是 DTX
client。本文档剩余章节描述的"YCode Debug Protocol（YDP）"是这套 DTX 之上的
**应用层 schema**（service / selector / aux 参数 / 命名 NSKeyedArchive 对象），
而非独立的传输或编码层。

设计原则：

```text
1. Visual Studio 侧负责 IDE 集成，不直接承载 LLDB 复杂逻辑。
2. Debug Host 侧负责 LLDB、设备、符号、调试会话管理。
3. 中间协议只表达调试语义，不泄漏 VS COM 对象，也不泄漏 LLDB 内部对象。
4. LLDB 深度集成，但运行在独立进程，避免拖垮 devenv.exe。
5. iOS / Android 设备链路由 YCode 自己控制，LLDB 只负责调试语义。
6. 协议要面向未来扩展，可复用到 VS Code、Rider、自研 IDE、命令行工具。
```

---

## 2. 总体架构

### 2.1 推荐架构

```text
+---------------------------------------------------+
| Visual Studio                                     |
|                                                   |
|  +---------------------------------------------+  |
|  | YCode VSIX                                  |  |
|  | - Project commands                          |  |
|  | - Device selector                           |  |
|  | - Run/debug configuration UI                |  |
|  | - Tool windows                              |  |
|  +---------------------------------------------+  |
|                                                   |
|  +---------------------------------------------+  |
|  | YCode AD7 Debug Engine                      |  |
|  | - Breakpoint binding                        |  |
|  | - Stack/thread/module mapping               |  |
|  | - VS debug window integration               |  |
|  | - Expression / variable adapter             |  |
|  +---------------------------------------------+  |
+-----------------------|---------------------------+
                        |
                        | IPC: DTX over Named Pipe / TCP / UDS
                        | (multi-channel; NSKeyedArchive payload)
                        | 详见 llvm-project/dtx/PLAN.md
                        v
+---------------------------------------------------+
| YCode.DebugHost.exe                               |
|                                                   |
|  +-------------------+   +---------------------+  |
|  | Session Manager   |   | LLDB Backend         |  |
|  | Device lifecycle  |   | liblldb SB API       |  |
|  +-------------------+   +---------------------+  |
|                                                   |
|  +-------------------+   +---------------------+  |
|  | Symbol Manager    |   | Formatter Manager    |  |
|  | dSYM / ELF / PDB  |   | STL / ObjC / Rust    |  |
|  +-------------------+   +---------------------+  |
+-----------------------|---------------------------+
                        |
          +-------------+-------------+
          |                           |
          v                           v
+-------------------+       +----------------------+
| iOS Device Bridge |       | Android Device Bridge |
| usbmux/lockdown   |       | adb/lldb-server       |
| debugserver       |       | JDWP optional         |
+-------------------+       +----------------------+
```

### 2.2 为什么不把 liblldb 直接加载到 Visual Studio 进程？

不建议让 VSIX 直接 load `liblldb.dll` 到 `devenv.exe`：

```text
1. LLDB 崩溃会直接带崩 Visual Studio。
2. Python formatter / expression evaluation 卡死会影响 IDE。
3. 符号加载和 DWARF 解析可能阻塞 UI。
4. LLVM / Clang / LLDB DLL 版本冲突风险高。
5. 插件更新、卸载、热加载会变复杂。
6. 用户会把 LLDB 内部问题归因成“插件导致 VS 崩溃”。
```

推荐方式：

```text
Visual Studio 原生体验
    +
独立 YCode.DebugHost.exe 承载 LLDB
```

这样可以实现崩溃隔离、版本隔离、进程重启、日志隔离和后端复用。

---

## 3. 模块拆分

### 3.1 Visual Studio Extension / VSIX

职责：

```text
1. 提供设备选择 UI。
2. 提供 iOS / Android Run Configuration。
3. 提供符号状态、设备状态、日志面板。
4. 启动 / 停止 Debug Host。
5. 与 AD7 Debug Engine 协作。
6. 提供菜单、工具栏、项目系统集成。
```

不应承担：

```text
1. 直接解析 DWARF。
2. 直接管理 LLDB 对象。
3. 直接处理 debugserver / lldb-server 协议。
4. 直接保存大量符号索引。
```

### 3.2 AD7 Debug Engine

职责：

```text
1. 实现 Visual Studio 调试器接口。
2. 将 VS 调试动作转换成 YCode Debug Protocol 请求。
3. 将 Host 返回的线程、调用栈、变量、断点事件转换成 VS 对象。
4. 管理 VS Pending Breakpoint / Bound Breakpoint。
5. 管理 IDebugProgram2 / IDebugThread2 / IDebugStackFrame2 / IDebugProperty2 等对象。
```

典型映射：

```text
IDebugProgram2
    -> YCode session / process

IDebugThread2
    -> threadId

IDebugStackFrame2
    -> frameId

IDebugProperty2
    -> variablesReference + variable path

IDebugPendingBreakpoint2
    -> client breakpoint id

IDebugBoundBreakpoint2
    -> backend breakpoint id

IDebugExpression2
    -> evaluate request
```

### 3.3 MixDevice.DebugHost.exe

职责：

```text
1. 管理一个或多个调试 Session。
2. 加载 liblldb，封装 LLDB SB API。
3. 管理 iOS / Android 设备链路。
4. 启动 debugserver / lldb-server。
5. 管理断点、内存断点、线程、调用栈、变量。
6. 管理符号缓存和符号加载。
7. 提供统一的协议服务。
8. 处理日志、诊断、错误恢复。
```

### 3.4 iOS Device Bridge

职责：

```text
1. 设备发现。
2. Pairing / lockdown。
3. Developer Mode 检查。
4. DeveloperDiskImage 挂载。
5. App 安装。
6. App 启动或 Attach。
7. debugserver 启动。
8. usbmux 端口转发。
9. 将 debugserver endpoint 交给 LLDB。
```

重要边界：

```text
设备生命周期：YCode 控制
调试语义：LLDB 控制
```

### 3.5 Android Device Bridge

职责：

```text
1. adb 设备发现。
2. ABI / SDK / package 检测。
3. adb forward。
4. push / 启动 lldb-server。
5. run-as / attach pid。
6. am start -D / native attach。
7. 将 lldb-server endpoint 交给 LLDB。
```

### 3.6 Symbol Manager

职责：

```text
1. 管理 iOS .dSYM。
2. 管理 Android unstripped .so / .dbg / .sym。
3. 按 UUID / Build ID 查找符号。
4. 管理本地 Symbol Cache。
5. 管理 Source Path Mapping。
6. 触发 LLDB 加载符号。
7. 向 UI 提供 missing / loading / loaded / mismatch 状态。
```

---

## 4. 中间协议设计：YCode Debug Protocol，YDP

### 4.1 协议定位

YDP 是 Visual Studio 插件与 Debug Host 之间的应用层调试 schema，**线协议
统一使用 DTX**（参见 [llvm-project/dtx/PLAN.md](dtx/PLAN.md)）。本节定义服务、
selector、命名对象，不重复定义传输与编码。

它应该：

```text
1. 表达调试语义，而不是 LLDB API。
2. 类似 DAP 风格的请求/响应/事件，但具体 selector / 对象由本文档定义。
3. 支持移动端扩展能力。
4. 支持 capability negotiation（通过 DTX 握手 channel）。
5. 支持 request / response / event 三种消息形态。
6. 支持 cancellation 和 timeout。
7. 支持大对象 stream / chunk 传输（基于 DTX 多 channel + 分片）。
```

传输层完全交由 DTX：

```text
线协议  ：DTX（NSKeyedArchive 负载，多 channel 复用，分片）
Transport：Named Pipe（Windows 同机首选）/ Unix Domain Socket（macOS/Linux）/ TCP
鉴权    ：在 DTX 握手 channel 上交换 capability + token（具体方案见 §5）
大对象  ：DTX 分片或专用 stream channel
代码生成：dtx-codegen（C++ / Qt / Kotlin / C# 客户端，C++ DebugHost dispatch）
```

> 不再使用 JSON-RPC / MessagePack-RPC 作为独立传输层；本文档之前版本中的
> "JSON 例子"在新方案下是**逻辑 schema**：每条 JSON 描述一次 DTX selector
> 调用的语义内容，由 DTX runtime 把它编码为 NSKeyedArchive 字典。

### 4.2 消息类型

DTX 原生支持三种消息形态，YDP 与之直接对齐：

| YDP 概念 | DTX 形态                                    | DTX `flags`               | 备注                              |
| -------- | ------------------------------------------- | ------------------------- | --------------------------------- |
| Request  | selector + AuxList + `expects_reply=1`      | payload `0x02`            | client → host                     |
| Response | reply 单对象                                 | payload `0x03` 或 `0x04`  | host → client，`conversation_id=1` |
| Event    | selector + AuxList + `expects_reply=0`，反向 channel | payload `0x02` | host → client，async              |

每条 IDE → Host 请求都对应一个 selector，放到该 service 所属的 channel 上。
`seq` / `request_seq` 由 DTX 的 `message_id` / `conversation_id` 自然承载，
schema 层不再单独编码。

逻辑示例（在本文档其它章节使用的 JSON 形式）：

```json
{
  "command": "stackTrace",
  "arguments": { "sessionId": "s-7f31", "threadId": 42 }
}
```

落到 DTX 层等价于：

```text
channel    : org.llvm.debughost.threads
selector   : stackTrace:
aux        : NSDictionary { sessionId="s-7f31", threadId=42 }
expects_reply : true
```

Reply 是一个命名 NSKeyedArchive 对象（如 `StackTraceResult`，对应原 JSON
`body`）；Event 是 host → client 反向 channel 上的 selector + aux，例如
`org.llvm.debughost.events` channel 上的 `stopped:` selector，aux 携带
`StoppedEvent` 字典。

### 4.3 服务与 channel 命名

DTX channel identifier 使用 `org.llvm.debughost.*` 命名空间，与
[dtx/PLAN.md §6.1](dtx/PLAN.md) 一致。建议的服务划分：

| 服务 channel                                  | 覆盖的 YDP 命令族                      |
| --------------------------------------------- | -------------------------------------- |
| `org.llvm.debughost.lifecycle`                | `initialize`、`shutdown`               |
| `org.llvm.debughost.session`                  | `session.*`                            |
| `org.llvm.debughost.device`                   | `device.*`                             |
| `org.llvm.debughost.app`                      | `app.*`                                |
| `org.llvm.debughost.execution`                | `execution.*`、`launch`、`attach`、`disconnect` |
| `org.llvm.debughost.breakpoints`              | `breakpoints.*`                        |
| `org.llvm.debughost.watchpoints`              | `watchpoints.*`                        |
| `org.llvm.debughost.threads`                  | `threads`、`stackTrace`、`scopes`、`variables` |
| `org.llvm.debughost.expressions`              | `evaluate`                             |
| `org.llvm.debughost.memory`                   | `memory.*`、`disassemble`              |
| `org.llvm.debughost.modules`                  | `modules`                              |
| `org.llvm.debughost.symbols`                  | `symbols.*`、`sourceMap.*`             |
| `org.llvm.debughost.trace`                    | `trace.*`、`perf.*`                    |
| `org.llvm.debughost.events`                   | 所有异步事件（host → client）          |

selector 命名遵循 [dtx/PLAN.md §6.1](dtx/PLAN.md) 的推导规则：0 参数为
lowerCamel，1 参数为 `method:`，多参数为 `method:firstArgRest:`。`command`
字段中带 `.` 的（例如 `breakpoints.setSource`）按其后缀生成 selector
（`setSource:`），由 channel 区分命令族。

### 4.4 与 DAP 的关系

建议采用：

```text
80% DAP 语义
20% YCode 移动端扩展
```

可直接沿用 DAP 风格的能力：

```text
initialize
launch
attach
disconnect
setBreakpoints
continue
pause
next
stepIn
stepOut
threads
stackTrace
scopes
variables
evaluate
readMemory
writeMemory
modules
loadedSources
```

YCode 扩展能力：

```text
device.list
device.prepareDebug
app.install
app.sign
ios.launchDebugServer
android.startLldbServer
symbols.lookup
symbols.add
symbols.scanProject
symbols.clearCache
trace.start
trace.stop
perf.sample
```

---

## 5. 协议生命周期

> 本节及之后章节中的 JSON 例子表示**逻辑 schema**。每条 `request` 在
> 实现层等价于一次 DTX selector 调用：`command` 决定 channel + selector，
> `arguments` 字段构成 selector 的命名 NSKeyedArchive 对象（aux 第一个 obj），
> `body` 字段则是 reply 的命名对象。在生产代码中，IDE 与 DebugHost 不直接
> 拼接 JSON，而是通过 `dtx-codegen` 生成的强类型 client / dispatcher 调用。

### 5.1 Initialize

`initialize` 不通过普通业务 channel，而是直接复用 DTX 的握手能力：连接建立
后，DebugHost 发出 `_notifyOfPublishedCapabilities:`（沿用 DTX 标准握手
selector），其 aux 携带本节 Response `body` 中的 `capabilities` 字段；IDE
回发同名 selector 报告自身能力，对应 Request 中的 `capabilities`。这一段
握手语义和 [dtx/PLAN.md §2](dtx/PLAN.md) 中描述的能力发布机制一致，无需
单独定义 channel。

逻辑 schema（IDE → Host）：

```json
{
  "seq": 1,
  "type": "request",
  "command": "initialize",
  "arguments": {
    "clientName": "YCode.VisualStudio",
    "clientVersion": "0.1.0",
    "protocolVersion": "1.0",
    "adapter": "vs-ad7",
    "capabilities": {
      "supportsMemoryReferences": true,
      "supportsProgress": true,
      "supportsCancellation": true
    }
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 1,
  "success": true,
  "body": {
    "hostName": "YCode.DebugHost",
    "hostVersion": "0.1.0",
    "protocolVersion": "1.0",
    "backends": ["lldb", "ios", "android"],
    "capabilities": {
      "supportsEvaluate": true,
      "supportsReadMemory": true,
      "supportsWriteMemory": true,
      "supportsDisassemble": true,
      "supportsWatchpoints": true,
      "supportsPerfTrace": true
    }
  }
}
```

### 5.2 Session Create

```json
{
  "seq": 2,
  "type": "request",
  "command": "session.create",
  "arguments": {
    "kind": "ios-native",
    "projectPath": "D:/Game/MyApp",
    "workspaceId": "vs-123",
    "capabilities": {
      "supportsEvaluate": true,
      "supportsReadMemory": true,
      "supportsDisassemble": true
    }
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 2,
  "success": true,
  "body": {
    "sessionId": "s-7f31",
    "backend": "lldb",
    "capabilities": {
      "supportsConditionalBreakpoints": true,
      "supportsFunctionBreakpoints": true,
      "supportsWatchpoints": true,
      "supportsSwiftExpression": false,
      "supportsObjCPo": true
    }
  }
}
```

---

## 6. Launch / Attach 设计

### 6.1 iOS Launch

```json
{
  "seq": 10,
  "type": "request",
  "command": "launch",
  "arguments": {
    "sessionId": "s-7f31",
    "platform": "ios",
    "deviceId": "00008110-xxxx",
    "bundleId": "com.company.game",
    "executablePath": "D:/build/MyApp.app/MyApp",
    "appPath": "D:/build/MyApp.app",
    "symbols": [
      {
        "kind": "dsym",
        "path": "D:/build/MyApp.app.dSYM"
      }
    ],
    "sourceMap": [
      {
        "localRoot": "D:/src/game",
        "remoteRoot": "/Users/build/src/game"
      }
    ],
    "stopAtEntry": false,
    "env": {
      "OS_ACTIVITY_DT_MODE": "disable"
    }
  }
}
```

Host 内部执行：

```text
1. 检查设备状态。
2. Pairing / lockdown。
3. 检查 Developer Mode。
4. 挂载 DeveloperDiskImage。
5. 安装 app。
6. 启动 debugserver。
7. 建立 usbmux 连接。
8. 创建 LLDB target。
9. gdb-remote connect。
10. 加载符号。
11. 绑定断点。
12. 发送 initialized / process.started 事件。
```

### 6.2 Android Attach

```json
{
  "seq": 11,
  "type": "request",
  "command": "attach",
  "arguments": {
    "sessionId": "s-82ad",
    "platform": "android",
    "deviceId": "R58Mxxxx",
    "packageName": "com.company.game",
    "pid": 23456,
    "abi": "arm64-v8a",
    "symbols": [
      {
        "kind": "elf",
        "path": "D:/build/libgame.so"
      }
    ]
  }
}
```

Host 内部执行：

```text
1. adb 检测设备。
2. 确认 ABI / package / pid。
3. push 或复用 lldb-server。
4. adb forward。
5. LLDB process connect。
6. 加载 ELF 符号。
7. 发送 process.started / stopped 事件。
```

---

## 7. 源码断点设计

### 7.1 设置源码断点

```json
{
  "seq": 20,
  "type": "request",
  "command": "breakpoints.setSource",
  "arguments": {
    "sessionId": "s-7f31",
    "source": {
      "path": "D:/src/game/Renderer.cpp"
    },
    "breakpoints": [
      {
        "id": "vs-bp-1001",
        "line": 128,
        "column": 1,
        "condition": "frameIndex == 120",
        "hitCondition": ">= 3",
        "logMessage": "frame={frameIndex}"
      }
    ]
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 20,
  "success": true,
  "body": {
    "breakpoints": [
      {
        "clientId": "vs-bp-1001",
        "backendId": "lldb-bp-3",
        "verified": true,
        "resolvedLocations": [
          {
            "address": "0x10482af20",
            "module": "MyApp",
            "line": 128,
            "column": 1
          }
        ]
      }
    ]
  }
}
```

### 7.2 Breakpoint Resolved Event

```json
{
  "type": "event",
  "event": "breakpoint.resolved",
  "body": {
    "sessionId": "s-7f31",
    "clientId": "vs-bp-1001",
    "backendId": "lldb-bp-3",
    "address": "0x10482af20"
  }
}
```

---

## 8. 运行控制

建议命令：

```text
execution.continue
execution.pause
execution.next
execution.stepIn
execution.stepOut
execution.restartFrame
execution.terminate
```

示例：

```json
{
  "seq": 30,
  "type": "request",
  "command": "execution.next",
  "arguments": {
    "sessionId": "s-7f31",
    "threadId": 42,
    "granularity": "statement"
  }
}
```

Stopped event：

```json
{
  "type": "event",
  "event": "stopped",
  "body": {
    "sessionId": "s-7f31",
    "reason": "step",
    "threadId": 42,
    "allThreadsStopped": true
  }
}
```

`reason` 枚举：

```text
breakpoint
step
pause
exception
signal
entry
watchpoint
dataBreakpoint
exited
unknown
```

---

## 9. 线程、调用栈、变量

### 9.1 Threads

```json
{
  "seq": 40,
  "type": "request",
  "command": "threads",
  "arguments": {
    "sessionId": "s-7f31"
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 40,
  "success": true,
  "body": {
    "threads": [
      {
        "id": 42,
        "name": "Main Thread",
        "state": "stopped",
        "queue": "com.apple.main-thread"
      },
      {
        "id": 43,
        "name": "RenderThread",
        "state": "stopped"
      }
    ]
  }
}
```

### 9.2 StackTrace

```json
{
  "seq": 41,
  "type": "request",
  "command": "stackTrace",
  "arguments": {
    "sessionId": "s-7f31",
    "threadId": 42,
    "startFrame": 0,
    "levels": 20
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 41,
  "success": true,
  "body": {
    "stackFrames": [
      {
        "id": 10001,
        "name": "Renderer::DrawFrame",
        "source": {
          "path": "D:/src/game/Renderer.cpp"
        },
        "line": 128,
        "column": 1,
        "instructionPointerReference": "0x10482af20",
        "moduleId": "mod-1"
      }
    ],
    "totalFrames": 12
  }
}
```

### 9.3 Scopes

```json
{
  "seq": 42,
  "type": "request",
  "command": "scopes",
  "arguments": {
    "sessionId": "s-7f31",
    "frameId": 10001
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 42,
  "success": true,
  "body": {
    "scopes": [
      {
        "name": "Locals",
        "variablesReference": 50001,
        "expensive": false
      },
      {
        "name": "Arguments",
        "variablesReference": 50002,
        "expensive": false
      },
      {
        "name": "Registers",
        "variablesReference": 50003,
        "expensive": true
      },
      {
        "name": "Globals",
        "variablesReference": 50004,
        "expensive": true
      }
    ]
  }
}
```

### 9.4 Variables

变量必须使用 lazy tree，不要一次性展开所有子项。

```json
{
  "seq": 43,
  "type": "request",
  "command": "variables",
  "arguments": {
    "sessionId": "s-7f31",
    "variablesReference": 50001,
    "start": 0,
    "count": 100
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 43,
  "success": true,
  "body": {
    "variables": [
      {
        "name": "frameIndex",
        "type": "int",
        "value": "120",
        "variablesReference": 0,
        "evaluateName": "frameIndex"
      },
      {
        "name": "mesh",
        "type": "Mesh*",
        "value": "0x600003a40120",
        "summary": "Mesh { vertices=2048, indices=6144 }",
        "variablesReference": 60001,
        "evaluateName": "mesh"
      }
    ]
  }
}
```

`variablesReference != 0` 表示可以继续展开。

重要规则：

```text
1. frameId、variablesReference 是短生命周期对象。
2. 每次进程 continue 后，旧 frameId / variablesReference 全部失效。
3. Host 负责缓存当前 stopped 状态下的变量树。
4. 大数组、大容器应支持分页。
```

---

## 10. 表达式求值

### 10.1 Evaluate

```json
{
  "seq": 50,
  "type": "request",
  "command": "evaluate",
  "arguments": {
    "sessionId": "s-7f31",
    "frameId": 10001,
    "expression": "mesh->GetVertexCount()",
    "context": "watch",
    "timeoutMs": 3000,
    "language": "cpp"
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 50,
  "success": true,
  "body": {
    "result": "2048",
    "type": "int",
    "variablesReference": 0,
    "sideEffects": "unknown"
  }
}
```

### 10.2 Context 类型

```text
watch
repl
hover
locals
conditionalBreakpoint
logpoint
```

建议限制：

```text
hover：尽量禁止有副作用表达式。
conditionalBreakpoint：短超时，避免卡住进程。
repl：可以允许函数调用，但要有超时。
watch：允许求值，但需要缓存和取消。
```

### 10.3 超时和取消

表达式求值必须支持：

```text
1. timeoutMs。
2. cancel request。
3. 独立 worker thread。
4. 必要时重启 LldbHost session。
```

---

## 11. 内存读写与反汇编

### 11.1 Memory Read

```json
{
  "seq": 60,
  "type": "request",
  "command": "memory.read",
  "arguments": {
    "sessionId": "s-7f31",
    "address": "0x10482af20",
    "count": 256
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 60,
  "success": true,
  "body": {
    "address": "0x10482af20",
    "data": "base64-encoded-data",
    "unreadableBytes": 0
  }
}
```

### 11.2 Disassemble

```json
{
  "seq": 61,
  "type": "request",
  "command": "disassemble",
  "arguments": {
    "sessionId": "s-7f31",
    "memoryReference": "0x10482af20",
    "instructionOffset": -8,
    "instructionCount": 32
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 61,
  "success": true,
  "body": {
    "instructions": [
      {
        "address": "0x10482af20",
        "instruction": "stp x29, x30, [sp, #-0x10]!",
        "symbol": "Renderer::DrawFrame",
        "source": {
          "path": "D:/src/game/Renderer.cpp"
        },
        "line": 128
      }
    ]
  }
}
```

---

## 12. 内存断点 / Watchpoint 设计

### 12.1 能力分类

内存断点分为：

```text
1. Hardware Watchpoint
   由 CPU 调试寄存器 / OS 调试接口实现。
   推荐第一版支持。

2. Software Memory Watchpoint
   通过单步、页保护、插桩、内存比较模拟。
   推荐后续实验性支持。
```

第一版优先做：

```text
address + size + write watchpoint
```

第二版扩展：

```text
expression / variable watchpoint
condition
hit count
old/new value shadow
read / readWrite watchpoint
alignment 自动扩大 + 软件过滤
```

### 12.2 Capability

```json
{
  "supportsWatchpoints": true,
  "supportsReadWatchpoints": false,
  "supportsWriteWatchpoints": true,
  "supportsReadWriteWatchpoints": true,
  "maxHardwareWatchpoints": 4,
  "supportedWatchpointSizes": [1, 2, 4, 8]
}
```

### 12.3 设置 Watchpoint

```json
{
  "seq": 100,
  "type": "request",
  "command": "watchpoints.set",
  "arguments": {
    "sessionId": "s-7f31",
    "watchpoints": [
      {
        "clientId": "vs-data-bp-1",
        "kind": "address",
        "address": "0x10482af20",
        "size": 4,
        "access": "write",
        "condition": "*(int*)0x10482af20 == 123",
        "hitCondition": ">= 1",
        "description": "Watch frameIndex write"
      }
    ]
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 100,
  "success": true,
  "body": {
    "watchpoints": [
      {
        "clientId": "vs-data-bp-1",
        "backendId": "lldb-watch-1",
        "verified": true,
        "hardware": true,
        "address": "0x10482af20",
        "size": 4,
        "access": "write"
      }
    ]
  }
}
```

### 12.4 触发事件

```json
{
  "type": "event",
  "event": "stopped",
  "body": {
    "sessionId": "s-7f31",
    "reason": "watchpoint",
    "threadId": 42,
    "watchpointId": "lldb-watch-1",
    "address": "0x10482af20",
    "access": "write",
    "oldValue": "0x0000007a",
    "newValue": "0x0000007b",
    "allThreadsStopped": true
  }
}
```

注意：

```text
1. oldValue / newValue 不一定总是可靠。
2. write watchpoint 触发时，平台不同，PC 可能停在写前或写后。
3. Host 可以维护 shadow copy 来提升体验，但要标记 valueChangeReliable。
```

### 12.5 Expression Watchpoint

```json
{
  "command": "watchpoints.set",
  "arguments": {
    "sessionId": "s-7f31",
    "watchpoints": [
      {
        "clientId": "vs-data-bp-2",
        "kind": "expression",
        "frameId": 10001,
        "expression": "this->frameIndex",
        "access": "write"
      }
    ]
  }
}
```

Host 处理流程：

```text
1. 用 LLDB evaluate / frame variable 找到变量地址。
2. 推导 size。
3. 调用 WatchAddress。
4. 返回绑定结果。
```

局部变量限制：

```text
1. stack local 的地址只在当前 frame 生命周期有效。
2. optimized-out variable 不可 watch。
3. register-only variable 不可 watch memory。
4. global / static / heap object 更适合长期 watch。
```

### 12.6 LLDB Host 映射方式

优先使用 SB API：

```cpp
lldb::SBError error;

lldb::SBWatchpoint watch = target.WatchAddress(
    address,
    size,
    /*read*/ false,
    /*write*/ true,
    error
);

if (!error.Success()) {
    // return WATCHPOINT_SET_FAILED
}

watch.SetCondition(condition.c_str());
watch.SetEnabled(true);
```

MVP 可先通过 LLDB command interpreter：

```text
watchpoint set expression -s 4 -w write -- 0x10482af20
```

---

## 13. 模块与符号管理

### 13.1 为什么需要 Symbol Cache

移动端调试器必须做符号缓存，否则每次调试都重新扫描、传输、索引符号，体验会很差。

尤其：

```text
iOS .dSYM：几十 MB 到数 GB。
Android unstripped .so / .dbg：几十 MB 到数 GB。
Unreal / Unity 大型项目符号体积更大。
```

调试启动时不应从设备反复拉符号，而应该：

```text
1. 从进程读取 module list。
2. 获取 module UUID / Build ID / arch / load address。
3. 查本地 Symbol Cache。
4. 查项目 build output。
5. 查用户配置 symbol paths。
6. 查 CI artifact / symbol server。
7. 命中后验证并加载。
8. 未命中则发 missing symbol event。
```

### 13.2 iOS 符号匹配

iOS 核心匹配键：

```text
Mach-O UUID + arch
```

典型符号：

```text
MyApp.app.dSYM
FrameworkName.framework.dSYM
App executable DWARF
Framework DWARF
```

推荐 cache key：

```text
platform = ios
uuid = Mach-O UUID
arch = arm64 / arm64e
objectName = MyApp
debugFileKind = dsym
```

目录结构：

```text
%LOCALAPPDATA%/YCode/SymbolCache/
  ios/
    uuid/
      A1B2C3D4-E5F6-.../
        arm64/
          MyApp.dSYM/
          metadata.json
```

metadata：

```json
{
  "platform": "ios",
  "uuid": "A1B2C3D4-E5F6-...",
  "arch": "arm64",
  "moduleName": "MyApp",
  "kind": "dsym",
  "originalPath": "D:/build/ios/MyApp.app.dSYM",
  "indexedAt": "2026-04-29T05:10:00Z",
  "lastUsedAt": "2026-04-29T05:20:00Z",
  "sizeBytes": 73400320,
  "sourceRoots": ["D:/src/game"],
  "debugInfoFormat": "dwarf"
}
```

### 13.3 Android 符号匹配

Android 核心匹配键：

```text
ELF Build ID + ABI
```

典型符号来源：

```text
unstripped .so
.so.dbg
separate debug file
NDK obj/local/arm64-v8a/*.so
CMake build/intermediates/cxx/Debug/.../*.so
Unity / Unreal symbols
Breakpad .sym
```

推荐 cache key：

```text
platform = android
buildId = ELF Build ID
abi = arm64-v8a / armeabi-v7a / x86_64
soname = libgame.so
debugFileKind = elf / dbg / sym
```

目录结构：

```text
%LOCALAPPDATA%/YCode/SymbolCache/
  android/
    build-id/
      ab/
        cdef1234567890.../
          arm64-v8a/
            libgame.so
            metadata.json
```

metadata：

```json
{
  "platform": "android",
  "buildId": "abcdef1234567890abcdef1234567890abcdef12",
  "abi": "arm64-v8a",
  "moduleName": "libgame.so",
  "kind": "unstripped-elf",
  "originalPath": "D:/project/build/intermediates/cxx/Debug/arm64-v8a/libgame.so",
  "indexedAt": "2026-04-29T05:12:00Z",
  "lastUsedAt": "2026-04-29T05:20:00Z",
  "sizeBytes": 214958080,
  "debugInfoFormat": "dwarf"
}
```

### 13.4 Symbol Manager Commands

建议协议命令：

```text
symbols.lookup
symbols.add
symbols.index
symbols.remove
symbols.scanProject
symbols.scanDirectory
symbols.status
symbols.clearCache
symbols.setSearchPaths
```

Lookup：

```json
{
  "seq": 200,
  "type": "request",
  "command": "symbols.lookup",
  "arguments": {
    "sessionId": "s-7f31",
    "module": {
      "platform": "ios",
      "moduleName": "MyApp",
      "uuid": "A1B2C3D4-E5F6-...",
      "arch": "arm64"
    }
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 200,
  "success": true,
  "body": {
    "status": "found",
    "symbolFile": {
      "kind": "dsym",
      "path": "C:/Users/Q/AppData/Local/YCode/SymbolCache/ios/uuid/A1B2.../arm64/MyApp.dSYM",
      "match": "exact"
    }
  }
}
```

Add：

```json
{
  "seq": 201,
  "type": "request",
  "command": "symbols.add",
  "arguments": {
    "sessionId": "s-7f31",
    "moduleId": "mod-1",
    "symbolFile": {
      "kind": "dsym",
      "path": "D:/build/MyApp.app.dSYM"
    },
    "cache": true
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 201,
  "success": true,
  "body": {
    "loaded": true,
    "cached": true,
    "cacheKey": "ios:uuid:A1B2C3D4:arm64"
  }
}
```

### 13.5 Symbol Events

Missing：

```json
{
  "type": "event",
  "event": "symbols.missing",
  "body": {
    "sessionId": "s-7f31",
    "moduleId": "mod-12",
    "moduleName": "libgame.so",
    "platform": "android",
    "buildId": "abcdef123456...",
    "abi": "arm64-v8a",
    "suggestedActions": [
      "Select unstripped libgame.so",
      "Scan build output",
      "Configure symbol search path"
    ]
  }
}
```

Status Changed：

```json
{
  "type": "event",
  "event": "symbols.statusChanged",
  "body": {
    "sessionId": "s-7f31",
    "moduleId": "mod-1",
    "status": "loaded"
  }
}
```

符号状态：

```text
missing
loading
loaded
mismatch
failed
system
```

### 13.6 Source Path Mapping

协议：

```json
{
  "command": "sourceMap.set",
  "arguments": {
    "sessionId": "s-7f31",
    "mappings": [
      {
        "remoteRoot": "/Users/buildbot/workspace/game",
        "localRoot": "D:/work/game"
      }
    ]
  }
}
```

缓存里也要保存 source map：

```json
{
  "sourceMap": [
    {
      "from": "/Users/buildbot/workspace/game",
      "to": "D:/work/game"
    }
  ]
}
```

### 13.7 Cache 策略

默认建议：

```text
Symbol Cache max size: 20GB
最近 90 天未使用自动清理
当前项目引用的符号不清理
允许用户手动清理
允许按项目配置 Cache 位置
```

可配置项：

```text
Cache location
Max cache size
Auto index symbols
Auto scan build output
Clear unused symbols
Clear all symbols
Symbol search paths
Remote symbol server URLs
```

---

## 14. Module 管理

### 14.1 Modules Command

```json
{
  "seq": 70,
  "type": "request",
  "command": "modules",
  "arguments": {
    "sessionId": "s-7f31"
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 70,
  "success": true,
  "body": {
    "modules": [
      {
        "id": "mod-1",
        "name": "MyApp",
        "path": "/private/var/containers/Bundle/Application/.../MyApp.app/MyApp",
        "localPath": "D:/build/MyApp.app/MyApp",
        "uuid": "A1B2C3D4-...",
        "architecture": "arm64",
        "loadAddress": "0x104800000",
        "symbolStatus": "loaded"
      }
    ]
  }
}
```

### 14.2 Module Loaded Event

```json
{
  "type": "event",
  "event": "module.loaded",
  "body": {
    "sessionId": "s-7f31",
    "module": {
      "id": "mod-1",
      "name": "MyApp",
      "uuid": "A1B2C3D4-...",
      "architecture": "arm64",
      "loadAddress": "0x104800000",
      "symbolStatus": "loading"
    }
  }
}
```

Host 收到 module loaded 后应自动：

```text
1. 提取 UUID / Build ID。
2. 查 Symbol Cache。
3. 查项目输出。
4. 自动加载匹配符号。
5. 发 symbols.statusChanged。
6. 未命中则发 symbols.missing。
```

---

## 15. 设备管理协议

### 15.1 Device List

```json
{
  "seq": 80,
  "type": "request",
  "command": "device.list",
  "arguments": {
    "platform": "ios"
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 80,
  "success": true,
  "body": {
    "devices": [
      {
        "id": "00008110-xxxx",
        "name": "QXZ iPhone",
        "platform": "ios",
        "osVersion": "18.4",
        "architecture": "arm64e",
        "connection": "usb",
        "developerMode": true,
        "paired": true,
        "debugAvailable": true
      }
    ]
  }
}
```

### 15.2 Device Prepare Debug

```json
{
  "seq": 81,
  "type": "request",
  "command": "device.prepareDebug",
  "arguments": {
    "platform": "ios",
    "deviceId": "00008110-xxxx",
    "requiredCapabilities": [
      "debugserver",
      "developerDiskImage"
    ]
  }
}
```

Response：

```json
{
  "type": "response",
  "request_seq": 81,
  "success": true,
  "body": {
    "ready": true,
    "actionsPerformed": [
      "mountedDeveloperDiskImage"
    ]
  }
}
```

### 15.3 Device Events

```text
device.connected
device.disconnected
device.statusChanged
device.prepareProgress
```

---

## 16. 事件系统

建议事件列表：

```text
initialized
process.started
process.exited
process.detached
stopped
continued
thread.started
thread.exited
module.loaded
module.unloaded
breakpoint.resolved
breakpoint.changed
symbols.statusChanged
symbols.missing
output
diagnostic
device.connected
device.disconnected
host.error
```

Output event：

```json
{
  "type": "event",
  "event": "output",
  "body": {
    "sessionId": "s-7f31",
    "category": "stdout",
    "output": "hello\n"
  }
}
```

Diagnostic event：

```json
{
  "type": "event",
  "event": "diagnostic",
  "body": {
    "sessionId": "s-7f31",
    "level": "warning",
    "source": "ios-bridge",
    "message": "DeveloperDiskImage already mounted"
  }
}
```

---

## 17. 错误模型

错误不能只返回字符串，要有标准 error code。

示例：

```json
{
  "type": "response",
  "request_seq": 10,
  "success": false,
  "message": "Failed to start debugserver",
  "error": {
    "code": "IOS_DEBUGSERVER_START_FAILED",
    "details": "lockdown service com.apple.debugserver failed",
    "recoverable": true,
    "suggestion": "Check Developer Mode and app signing entitlements."
  }
}
```

错误码分层：

```text
HOST_*
DEVICE_*
IOS_*
ANDROID_*
LLDB_*
SYMBOL_*
BREAKPOINT_*
WATCHPOINT_*
EVALUATE_*
TIMEOUT_*
PROTOCOL_*
```

建议错误码：

```text
IOS_DEVICE_NOT_PAIRED
IOS_DEVELOPER_MODE_DISABLED
IOS_DDI_MOUNT_FAILED
IOS_DEBUGSERVER_START_FAILED
ANDROID_ADB_NOT_FOUND
ANDROID_RUN_AS_FAILED
LLDB_CONNECT_FAILED
LLDB_EXPRESSION_TIMEOUT
SYMBOL_UUID_MISMATCH
SYMBOL_BUILD_ID_MISMATCH
BREAKPOINT_UNRESOLVED
WATCHPOINT_LIMIT_REACHED
WATCHPOINT_UNSUPPORTED_SIZE
WATCHPOINT_VARIABLE_NO_ADDRESS
```

用户提示示例：

```text
Cannot set memory breakpoint: hardware watchpoint limit reached.
This device supports 4 hardware watchpoints. Delete an existing memory breakpoint and try again.
```

```text
Cannot watch this variable because it has no stable memory address.
It may be optimized out or stored in a register.
Try building with debug symbols and lower optimization.
```

---

## 18. 取消、超时与进度

### 18.1 Cancel

```json
{
  "seq": 99,
  "type": "request",
  "command": "cancel",
  "arguments": {
    "requestSeq": 50
  }
}
```

### 18.2 Timeout 建议

```text
短请求：1-3s
表达式求值：3-10s
符号加载：30-120s
设备 prepare：30-60s
launch / attach：30-90s
```

### 18.3 Progress Event

```json
{
  "type": "event",
  "event": "progress",
  "body": {
    "sessionId": "s-7f31",
    "operationId": "op-symbol-load-1",
    "title": "Loading symbols",
    "message": "Indexing MyApp.dSYM",
    "percentage": 42
  }
}
```

---

## 19. 大对象传输

以下对象不要直接塞进 JSON：

```text
memory dump
trace data
performance capture
large variable array
large log stream
symbol index
```

建议返回 stream handle：

```json
{
  "body": {
    "streamId": "stream-abc",
    "size": 104857600,
    "contentType": "application/octet-stream"
  }
}
```

传输方式：

```text
Named Pipe stream
TCP stream
临时文件 mmap
chunked RPC
```

---

## 20. Host 内部对象 ID 设计

协议对象不要暴露 LLDB 指针，也不要暴露 VS COM 对象。

统一 ID：

```text
sessionId: string
processId: number/string
threadId: number/string
frameId: number/string
variablesReference: number
moduleId: string
breakpointId: string
watchpointId: string
streamId: string
operationId: string
```

生命周期规则：

```text
1. sessionId 在调试会话内稳定。
2. threadId 在进程生命周期内尽量稳定。
3. frameId 每次 stopped 后重新生成。
4. variablesReference 每次 stopped 后重新生成。
5. continue 后旧 frameId / variablesReference 全部失效。
6. breakpointId / watchpointId 在绑定生命周期内稳定。
```

---

## 21. LLDB 集成策略

### 21.1 推荐使用 SB API

```cpp
lldb::SBDebugger debugger = lldb::SBDebugger::Create();
lldb::SBTarget target = debugger.CreateTarget(...);
lldb::SBProcess process = target.ConnectRemote(...);
```

优点：

```text
1. 比 lldb_private API 稳定。
2. 官方外部集成 API。
3. 可以做结构化对象映射。
```

### 21.2 可使用 command interpreter 做 MVP

例如 watchpoint：

```text
watchpoint set expression -s 4 -w write -- 0x10482af20
```

优点：

```text
1. 快速验证。
2. 覆盖 LLDB 命令行已有能力。
```

缺点：

```text
1. 需要解析文本。
2. 错误结构化差。
3. 长期维护不如 SB API。
```

### 21.3 不推荐直接使用 lldb_private

`lldb_private::*` 能力强，但 API 不稳定，升级 LLVM/LLDB 会带来大量维护成本。

---

## 22. iOS / Android 平台限制

### 22.1 iOS

限制：

```text
1. 需要 Developer Mode。
2. 需要可调试签名和 entitlement。
3. 需要 DeveloperDiskImage / debugserver 能力。
4. watchpoint 数量有限。
5. read watchpoint 支持不一定稳定。
6. Swift expression evaluation 在 Windows 上难度较高。
```

建议阶段：

```text
Phase 1：C / C++ / ObjC 基础调试。
Phase 2：ObjC po / Foundation 类型展示。
Phase 3：Swift 栈和基本变量。
Phase 4：Swift expression evaluation。
```

### 22.2 Android

限制：

```text
1. 受 adb / ptrace / SELinux / app sandbox 影响。
2. lldb-server 版本需要和 LLDB client 兼容。
3. ABI 需要匹配。
4. Java/Kotlin 层调试需要 JDWP，是另一套链路。
```

建议：

```text
Phase 1：native C/C++ 调试。
Phase 2：JNI 混合栈增强。
Phase 3：JDWP / ART 集成。
```

---

## 23. MVP 范围

### 23.1 MVP Command 集合

```text
initialize
shutdown

session.create
launch
attach
disconnect

breakpoints.setSource
execution.continue
execution.pause
execution.next
execution.stepIn
execution.stepOut

threads
stackTrace
scopes
variables
evaluate

modules
symbols.lookup
symbols.add
symbols.scanDirectory
sourceMap.set

memory.read
disassemble

watchpoints.set
watchpoints.remove
watchpoints.list

device.list
device.prepareDebug

cancel
```

### 23.2 MVP Event 集合

```text
initialized
process.started
process.exited
stopped
continued
thread.started
thread.exited
module.loaded
breakpoint.resolved
symbols.statusChanged
symbols.missing
output
diagnostic
host.error
```

### 23.3 MVP 功能

```text
1. iOS USB 设备发现。
2. Android adb 设备发现。
3. iOS app install / launch / debugserver attach。
4. Android lldb-server attach。
5. 源码断点。
6. Step in / over / out。
7. Threads / stack / locals。
8. Watch / evaluate。
9. Memory read。
10. Disassemble。
11. Address write watchpoint。
12. iOS dSYM cache。
13. Android Build ID symbol cache。
14. Source path mapping。
15. VS 调试窗口集成。
```

---

## 24. 目录结构建议

### 24.1 Protocol Schema

YDP schema 不再独立成仓，而是直接落在 `llvm-project/dtx/schema/` 下，
与 DTX runtime / codegen 共享同一份 TableGen IDL（详见
[dtx/PLAN.md §3](dtx/PLAN.md)）。多语言客户端代码由 `dtx-codegen` 自动产出。

```text
llvm-project/dtx/
  schema/
    DTXBase.td
    DebugHostCore.td      # process / threads / breakpoints / expressions / memory / modules / symbols
    DebugHostEvents.td    # stop / output / module loaded / symbol status 等异步事件
  docs/
    services/lifecycle.md
    services/breakpoints.md
    services/watchpoints.md
    services/variables.md
    services/symbols.md
    services/mobile-ios.md
    services/mobile-android.md
  fixtures/
    frames/               # IDE ↔ DebugHost 录制流量，用于 round-trip 测试
  runtime/
    cpp-llvm/             # DebugHost 直接链接（client + server/dispatch）
    cpp-qt/               # Qt IDE 客户端 runtime
    kotlin/               # JetBrains 平台客户端 runtime
    csharp/               # Visual Studio / .NET 客户端 runtime
  tools/dtx-codegen/      # 由 .td 生成各语言 client（Qt/Kotlin/C# client-only）
                          # + DebugHost dispatch（仅 cpp-llvm）
```

### 24.2 Debug Host

```text
llvm-project/lldb/tools/lldb-mixdev/
  CMakeLists.txt              # 构建 ycode-debughost，输出名 YCode.DebugHost.exe
  Main.cpp                    # DTX TCP server 入口，注册 generated DebugHostServer
  DebugHostServices.h/.cpp    # 聚合所有 DebugHostCore.td generated processor
  LldbBackend.h/.cpp          # liblldb SB API 会话、launch/attach、断点、线程、变量、内存
  MixDeviceBridge.h/.cpp      # mix_device.h C FFI 的 C++ RAII/对象化边界
  Core/                       # 后续扩展：SessionManager / EventBus / OperationManager
  Services/                   # 后续拆分：每个 I<Service>Processor 一个实现文件
    LifecycleService          # 当前在 DebugHostServices.cpp 中实现
    DeviceService             # 当前在 DebugHostServices.cpp + MixDeviceBridge 中实现
    SessionService            # 当前通过 LldbBackend 创建 / 关闭 SBDebugger session
    ExecutionService          # 当前通过 SBTarget / SBProcess launch、attach、continue、pause
    BreakpointService         # 当前通过 SBTarget::BreakpointCreateByLocation 设置源码断点
    ThreadsService            # 当前通过 SBProcess / SBThread / SBFrame 暴露 threads、stack、scopes、variables
    ExpressionsService        # 当前通过 SBFrame::EvaluateExpression 求值
    MemoryService             # 当前通过 SBProcess::ReadMemory / SBTarget::ReadInstructions
    ModulesService            # 后续 schema 扩展
    SymbolsService            # 后续 schema 扩展
    TraceService              # 后续 schema 扩展
  Backends/
    LldbBackend               # 当前已接入 liblldb SB API；mix URL 通过 gdb-remote 连接
    AndroidBackend
    IosBackend
  Symbols/
    SymbolManager
    SymbolCache
    DsymScanner
    ElfBuildIdScanner
  Diagnostics/
    Logging
    CrashRecovery
```

LLDB gdb-remote 的 mix 连接被接在 LLDB Host 层，而不是只放在 DebugHost
外层做 TCP 转发：

```text
lldb/include/lldb/Host/ConnectionRemoteIOS.h
lldb/source/Host/common/ConnectionRemoteIOS.cpp
lldb/source/Plugins/Process/gdb-remote/ProcessGDBRemote.cpp
lldb/source/Plugins/Platform/gdb-server/PlatformRemoteGDBServer.cpp
```

`ConnectionRemoteIOS` 识别 `ios://<deviceId>`、`android://<deviceId>`、
`mix-ios://<deviceId>`、`mix-android://<deviceId>` 这类 URL；连接时先调用
`mix_connect_device`，再调用 `mix_device_create_lldb_connection`，后续
`Read` / `Write` / `InterruptRead` 分别转到 `mix_lldb_connection_read`、
`mix_lldb_connection_write`、`mix_lldb_connection_interrupt_read`。因此
`ProcessGDBRemote::ConnectToDebugserver` 和 `platform connect` 可以直接把
mix URL 当作 gdb-remote transport 使用。

当前 MVP 的运行形态：

```text
YCode.DebugHost.exe --print-port --serve-one
  1. 监听 loopback TCP，并输出 PORT=<port>。
  2. 通过 dtx::ServerSession 读取一条 DTX selector request。
  3. 使用 dtx-codegen 生成的 DebugHostServer 按 channel id + selector dispatch。
  4. Lifecycle.initialize 返回 LLDB 与 mix_device capability 字典。
  5. Device.list 调用 mix_list_devices，并把 uid/name/platform/osVersion/features
     转成 NSObject 字典数组。
  6. Device.prepareDebug 调用 mix_device_prepare_tools，并返回 before/after state
     与 progress 列表。
  7. Session / Execution / Breakpoints / Threads / Expressions / Memory 已注册
     processor，并进入 LldbBackend 的真实 SB API 路径；缺少 session / target /
     process 时返回结构化错误，不再返回未接线 stub。
  8. launch / attach 请求若携带 platform=ios/android + deviceId，或显式 connectUrl，
     会通过 SBTarget::ConnectRemote(..., "gdb-remote") 进入 mix LLDB 连接。
```

当前本地构建命令：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
cmake -G Ninja -S F:\llvm-project\llvm -B F:\llvm-project\.build\x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DLLVM_ENABLE_PROJECTS=clang;dtx;lldb ^
  -DLLVM_TARGETS_TO_BUILD=X86;AArch64 ^
  -DLLDB_ENABLE_PYTHON=OFF -DLLDB_ENABLE_LUA=OFF -DLLDB_ENABLE_LIBXML2=OFF ^
  -DLLDB_ENABLE_LZMA=OFF -DLLDB_ENABLE_CURSES=OFF ^
  -DYCODE_MIXDEVICE_ROOT=C:\Users\Station\Desktop\IrisBuild
ninja -C F:\llvm-project\.build\x64 ycode-debughost
```

DebugHost 的 C# .NET Framework 4.7.2 client 通信测试位于
`dtx/test/csharp/YCodeDebugHostClientNet472`。测试会启动
`YCode.DebugHost.exe --print-port`，使用 `dtx-codegen` 生成的 C# client 调用
`initialize`、`device.list`、`device.prepareDebug`、`session.create/close`、执行、
断点、线程、表达式和内存服务的基础成功/错误路径。可通过构建树目标直接运行：

```bat
ninja -C F:\llvm-project\.build\x64 ycode-debughost-csharp-net472-test
```

如果要带真实设备验证 `device.prepareDebug`，也可以手动运行：

```bat
F:\llvm-project\dtx\test\csharp\YCodeDebugHostClientNet472\bin\Debug\net472\YCodeDebugHostClientNet472.exe ^
  F:\llvm-project\.build\x64\bin\YCode.DebugHost.exe --device-id <device-id> --platform ios
```

后续应把 `DebugHostServices.cpp` 中的 processor 拆到 `Services/`，并把 TCP
transport 替换/补充为 Windows Named Pipe，供 VSIX 同机连接。

### 24.3 Visual Studio 插件

```text
YCode.VisualStudio/
  Vsix/
  DebugEngine/
    AD7Engine
    AD7Program
    AD7Thread
    AD7StackFrame
    AD7Property
    AD7Breakpoint
  Protocol/
    DebugClient                # 调用 dtx-codegen 生成的 C# client（仅 client）
                               # 内部依赖 csharp DTX runtime（runtime/csharp/）
  UI/
    DeviceWindow
    SymbolStatusWindow
    DiagnosticsWindow
```

当前 VSIX 接入状态：

```text
src/ycode/debugger/VSDebugger/
  Protocol/Dtx/                  # 内嵌 net472 可编译的 DTX C# runtime
  Protocol/Generated/            # dtx-codegen 生成的 DebugHost C# client/types
  Protocol/DebugHostClientSession.cs
                                 # 启动 YCode.DebugHost.exe --print-port，
                                 # 建立 DTX TCP 连接并调用 initialize / device.prepareDebug
  DebugBridge/MixDebugLauncher.cs
                                 # Launch / Attach 进入旧 MIEngine 前先通过 DebugHost
                                 # 做 device.prepareDebug；优先把返回的 connectUrl
                                 # 写入 MIEngine 的 miDebuggerServerAddress。
                                 # 如果 connectUrl 缺失，则回退到现有
                                 # DebugServerForwarder + loopback TCP 路线。
```

`YCode.DebugHost.exe` 解析顺序为：

```text
1. 环境变量 YCODE_DEBUGHOST_EXE
2. VSIX/MixDebugger.dll 同目录下的 YCode.DebugHost.exe
3. 开发机 fallback: F:\llvm-project\.build\x64\bin\YCode.DebugHost.exe
```

VSIX 构建时会在 `dtx-codegen.exe` 和 `DebugHostCore.td` 可用时重新生成
`Protocol/Generated/*.g.cs`，并会把 `YCode.DebugHost.exe`、`liblldb.dll` 和
`debugger_mix_device.xml` 打进 VSIX。`DebugHostClientSession` 现在由
`DebugSessionHandle` 保活，避免 `device.prepareDebug` 完成立刻 shutdown host。

因此当前阶段已经把 VS 插件接到 DebugHost 协议上，并让 DebugHost 产出的
`connectUrl` 进入 MIEngine 连接配置；但实际 VS 调试窗口、断点、线程和变量显示
仍由旧 MIEngine 承担。后续 AD7 迁移时再把 `session.*`、`execution.*`、threads、
variables、events 等服务逐步从 MIEngine 搬到 DebugHost。

### 24.4 其它 IDE 插件

```text
YCode.JetBrains/             # IntelliJ / Rider 插件，依赖 runtime/kotlin/ + Kotlin client
YCode.QtIDE/                 # Qt 工具或 Qt Creator 扩展，依赖 runtime/cpp-qt/ + Qt client
YCode.CliTools/              # 命令行调试器 / 自动化测试，依赖 runtime/cpp-llvm/ + LLVM C++ client
```

---

## 25. 开发阶段规划

### Phase 0：协议和 Host 骨架

```text
1. 跟进 dtx/PLAN.md 的里程碑，落地 cpp-llvm DTX runtime（M0–M3）。（已完成 MVP）
2. 在 dtx/schema/ 下编写 DebugHostCore.td / DebugHostEvents.td。（已完成 MVP）
3. 在 lldb/tools/lldb-mixdev 中实现 YCode.DebugHost.exe，内嵌 dtx::Server，
   注册 lifecycle / session / execution / breakpoints / threads / expressions /
   memory / device 等 DebugHostCore 服务。（已完成 MVP：全部 service 已注册，
   device.list 接入 mix_device；session / execution / breakpoints / threads /
   expressions / memory 已接入 liblldb SB API）
4. 通过 dtx-codegen 生成 C# client，VS 插件接入并跑通 initialize /
   session.create / shutdown / 一个 stopped 事件。
5. 实现 DebugHost 日志和崩溃恢复。
6. 实现 VS 插件启动 Host（Named Pipe Transport）。
```

### Phase 1：LLDB 基础调试

```text
1. 集成 liblldb SB API。（已完成基础接入）
2. 实现 launch / attach 基础流程。（已完成本地 SBTarget / SBProcess 路径）
3. 实现 breakpoint。（已完成源码断点基础路径）
4. 实现 continue / pause / step。（continue / pause 已完成；step 待扩展 schema）
5. 实现 threads / stackTrace。（已完成基础路径）
6. 实现 scopes / variables。（已完成 variablesReference 基础路径）
7. 实现 evaluate。（已完成 frame expression 基础路径）
8. 实现 readMemory / disassemble。（已完成基础路径）
9. 实现 stopped / continued / exited event。
10. 实现 source map。
```

### Phase 2：移动设备链路

```text
1. iOS device bridge。
2. Android device bridge。
3. iOS debugserver 启动。
4. Android lldb-server 启动。
5. 设备状态和错误诊断。
```

### Phase 3：符号缓存

```text
1. iOS Mach-O UUID 扫描。
2. iOS dSYM cache。
3. Android ELF Build ID 扫描。
4. Android symbol cache。
5. 自动加载符号。
6. Missing symbol UI。
7. Source path mapping。
```

### Phase 4：内存调试增强

```text
1. memory.read。
2. disassemble。
3. address write watchpoint。
4. watchpoint event。
5. expression watchpoint。
6. old/new value shadow。
```

### Phase 5：产品体验增强

```text
1. 更好的变量 formatter。
2. STL / ObjC / Rust pretty display。
3. Swift 基础变量。
4. 日志面板。
5. 设备诊断向导。
6. 符号缓存管理 UI。
7. 性能分析联动。
```

---

## 26. 关键风险与规避

### 26.1 LLDB 崩溃风险

规避：

```text
1. 进程外 Host。
2. Host watchdog。
3. Session-level restart。
4. 崩溃日志收集。
```

### 26.2 表达式求值卡死

规避：

```text
1. timeoutMs。
2. cancel request。
3. 限制 hover / conditional expression。
4. 必要时重启 session。
```

### 26.3 符号不匹配

规避：

```text
1. iOS 使用 Mach-O UUID 精确匹配。
2. Android 使用 ELF Build ID 精确匹配。
3. 不允许只靠文件名匹配。
4. UI 明确显示 mismatch。
```

### 26.4 Watchpoint 数量和对齐限制

规避：

```text
1. capability 返回 maxHardwareWatchpoints。
2. supportedWatchpointSizes 明确声明。
3. 失败时给出可恢复建议。
4. 后续支持扩大范围 + 软件过滤。
```

### 26.5 Swift 调试难度

规避：

```text
1. 不在第一版承诺完整 Swift expression。
2. 第一版支持 C/C++/ObjC。
3. Swift 分阶段实现：栈、基本变量、表达式。
```

---

## 27. 最终建议

YCode 的 Visual Studio 移动调试器应采用：

```text
Visual Studio AD7 Debug Engine
    +
DTX 协议（线协议） + YDP schema（应用层）
    +
进程外 MixDevice.DebugHost.exe
    +
liblldb SB API
    +
YCode 自研 iOS / Android Device Bridge
    +
产品级 Symbol Manager / Symbol Cache
```

一句话总结：

```text
VS 侧做体验，Host 侧做调试，协议层做稳定边界，Symbol Cache 做专业体验，设备链路做核心壁垒。
```

这个方案既能获得 LLDB 的强大调试能力，又不会让 Visual Studio 进程承担 LLDB 的复杂性；同时通过自研设备桥接和符号管理，保留 YCode 在 Windows 上移动开发工具链的核心竞争力。
