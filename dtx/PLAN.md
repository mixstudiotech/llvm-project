# DTX 协议在 llvm-project 中的实现规划

本文档规划在 `llvm-project/dtx/` 子项目中以 C++ 实现一套 **DebugHost ↔ IDE**
之间通用的 RPC 协议。线协议沿用 Apple DTX
（Distributed eXecution / DeveloperToolset eXchange）的帧格式与 NSKeyedArchive
负载语义，以复用已有 Rust 实现（IrisBuild 仓库
[rust-crates/dtx/](../../IrisBuild/rust-crates/dtx/)）的工具链、fixtures 与
codegen 经验，但**不**面向 iOS 设备，也不与 `com.apple.instruments.*` 等
设备侧服务通信。

> 2026-04-30 路线更新：Visual Studio 插件当前继续使用既有
> **MIEngine + lldb-mi + DebugServerForwarder** 调试链路。DTX 子项目仍作为
> DebugHost 与多 IDE 之间的长期 RPC 基础设施推进，但不再是 VS 插件当前阶段
> 启动、Attach、断点、线程、调用栈、变量等功能的前置依赖。VS 侧近期工作应
> 聚焦在现有 MIEngine 路径的项目系统 F5、Attach-to-Process、符号路径和诊断
> 完整性上。

> 2026-04-30 实现状态：`dtx/` 已开始落地 standalone MVP。当前包含
> `runtime/cpp-llvm` 的 DTX header / payload / fragment / aux 基础实现、
> `schema/DebugHostCore.td` 与 `DebugHostEvents.td`、以及 `dtx-codegen`
> 的 C++ client/service/types 生成路径。`dtx` 也已注册为 LLVM known project，
> 后续可通过 `LLVM_ENABLE_PROJECTS=dtx` 接入主构建；当前验证入口仍是
> standalone CMake。
> 后续补充已加入：`KeyedArchiver` 接口隔离了 aux object 归档层（当前为
> deterministic 项目内二进制编码，后续替换为完整 NSKeyedArchive/bplist）；
> `Connection` 已能通过抽象 `Transport` 发送 selector request 并读取 object
> reply；C++ codegen 已生成 client、processor interface、service desc 与
> selector dispatch glue。当前又补齐了 TCP transport、`ServerSession`、生成的
> `DebugHostServer` bundle，并通过 `dtx-debughost-tcp-smoke` 在 localhost 上跑通
> generated client → TCP → generated server → processor → reply 的 DebugHost
> 协议闭环。

> 2026-04-30 C# 进展：已新增 `runtime/csharp` 的 .NET Framework 4.7.2 /
> .NET 8 client runtime，
> 覆盖 DTX header / payload / fragment、aux 参数、当前项目内
> `KeyedArchiver` MVP 格式、TCP loopback transport 与 `Connection.CallAsync`。
> `dtx-codegen --lang csharp` 已可生成 `DebugHostTypes.g.cs` 和
> `DebugHostClient.g.cs`，生成的 client 暴露 `Task<T>` + `CancellationToken`
> 风格 API。`dtx-csharp-client-to-cpp-server` smoke test 会构建 C# runtime 与
> 生成代码，启动现有 C++ DebugHost TCP server，并验证 C# client → TCP →
> generated C++ server 的真实 DebugHost 调用链路。后续又将 C# runtime 改为
> `net472;net8.0` multi-target，并新增 `DtxCSharpGeneratedNet472` compile-only
> 检查，确保生成的 C# DebugHost client/types 能被 .NET Framework 4.7.2 项目消费。

> 2026-04-30 Kotlin 进展：已新增 `runtime/kotlin` 的 JVM client runtime，
> 覆盖同一组 DTX header / payload / fragment、aux 参数、项目内
> `KeyedArchiver` MVP 格式、TCP loopback transport 与阻塞式 `Connection.call`。
> `dtx-codegen --lang kotlin` 已可生成 `DebugHostTypes.kt` 和
> `DebugHostClient.kt`。`dtx-kotlin-client-to-cpp-server` smoke test 会在
> `kotlinc` 可用时编译 Kotlin runtime 与生成代码，启动 C++ DebugHost TCP
> server，并验证 Kotlin client → TCP → generated C++ server 的真实调用链路。

---

## 1. 背景与动机

DTX 在本项目中被复用为 DebugHost 与 IDE 之间的本地/远程 RPC 协议。其线协议
天然契合调试器需求：

- 帧式二进制头 + NSKeyedArchiver 编码的对象负载，支持任意结构化数据。
- 通过 selector 字符串 + auxiliary 参数列表表达类似 Objective-C 的方法调用。
- 单连接多 channel 复用：每个调试领域（process control、threads、breakpoints、
  expressions、events stream 等）一个 channel，channel 内同步/异步消息分流。
- 支持分片（fragment）以承载大对象（变量树、内存 dump、Backtrace）。

llvm-project 内的需求场景：

1. **DebugHost** 是承载调试逻辑的常驻进程，内部可嵌入 LLDB / mixdevice 等后端；
   它需要向多种 IDE 暴露统一的调试 RPC。
2. **IDE 客户端**包括 Visual Studio (.NET)、JetBrains 平台 (Kotlin/JVM)、
   基于 Qt 的 IDE / Qt Creator 扩展，以及若干基于 LLVM 工具链的 CLI / 测试器
   （C++）。每种 IDE 通过同一套 DTX schema 与 DebugHost 交互。
3. 与 IrisBuild 中的 Rust 实现保持线协议一致，以便：
   - 复用 Rust 端已经能跑通的抓包 fixtures 做字节级互测。
   - 让现有 Rust 工具（如 mixdevice 衍生组件）可以无缝充当 DebugHost 或测试桩。

> 备注：该模块**不**依赖任何 LLVM IR/Codegen，只是寄宿在 monorepo 中复用
> LLVM 构建/测试基础设施。也**不**直接连接 iOS / macOS 设备：与设备的交互
> 由 DebugHost 内部的其它模块负责，DTX 只是其北向接口。

---

## 2. 与 Rust 参考实现的对齐范围

参考 [base.rs](../../IrisBuild/rust-crates/dtx/src/base.rs)、
[ns.rs](../../IrisBuild/rust-crates/dtx/src/ns.rs)、
[services/](../../IrisBuild/rust-crates/dtx/src/services/)：

| Rust 类型 / 函数                       | C++ 对应                                          | 在本项目的角色                                |
| -------------------------------------- | ------------------------------------------------- | --------------------------------------------- |
| `DTXMessageHeader`                     | `dtx::MessageHeader`（POD，magic `0x1F3D5B79`）   | 共用                                          |
| `DTXMessagePayloadHeader`              | `dtx::PayloadHeader`                              | 共用                                          |
| `DTXFragment`                          | `dtx::Fragment`                                   | 共用                                          |
| `MessageAux` / `DTXArg`                | `dtx::AuxList` / `dtx::Arg`                       | 共用                                          |
| `NSObject` / `NSKeyedArchiver`         | `dtx::ns::Object` / `dtx::ns::KeyedArchiver`      | 共用                                          |
| `Transport` trait                      | `dtx::Transport` 抽象基类                         | 本项目限定为本地/远程主机间传输（见 §7）      |
| `Service<T,S>`（client）               | `dtx::Connection`                                 | **IDE 侧**                                    |
| `Server` / `Channel` / `ServerService` | `dtx::Server` / `dtx::Channel` / `dtx::IService`  | **DebugHost 侧**                              |
| `#[service]` 宏 + `dtx_macros`         | tablegen + 多语言 codegen backend                 | DebugHost / IDE 双方                          |

线协议常量必须与 Rust 实现逐字节一致：magic、header_length=0x20、
auxiliary 类型 tag（2/3/4/5/6/10）、`flags` 取值（0x00/0x01/0x02/0x03/0x04/0x05）、
压缩位 `(flags & 0xFF000) >> 12`。

---

## 3. 目录与构建集成

`llvm-project/dtx/` 下分三大块：**LLVM C++ 主实现 + 多语言 client runtime +
codegen / schema / shared fixtures**。所有语言 runtime 共享同一份 schema 与
抓包 fixtures，互不依赖对方的构建系统。

```
llvm-project/dtx/
├── CMakeLists.txt              # 入口；按 LLVM_ENABLE_PROJECTS=dtx 启用 LLVM 部分
├── PLAN.md                     # 本文档
├── README.md                   # 总览 + 各语言 runtime 入口指引
│
├── schema/                     # 多语言共享 IDL（TableGen）
│   ├── DTXBase.td              # Service/Method/Arg/Type 基类
│   ├── DebugHostCore.td        # process control / threads / breakpoints / expressions
│   └── DebugHostEvents.td      # stop event / output stream / lifecycle 等异步事件
│
├── fixtures/                   # 多语言共享抓包 / 录制流量（git LFS）
│   ├── bplist/                 # 复用 IrisBuild test_data 的副本
│   ├── frames/                 # 完整 DTX 帧二进制录制
│   └── README.md               # fixtures 的来源、采集脚本、版本
│
├── docs/                       # 协议、对象、selector 速查；面向 4 套 runtime
│   ├── wire-format.md
│   ├── nskeyed-archive.md
│   └── services/*.md
│
├── runtime/                    # 各语言基础 runtime 实现（与 codegen 解耦）
│   │
│   ├── cpp-llvm/               # LLVM C++ runtime —— LLVM 构建图内
│   │   ├── CMakeLists.txt
│   │   ├── include/dtx/
│   │   │   ├── Protocol.h      # 头部 / payload / flags 常量与 POD
│   │   │   ├── Fragment.h      # 分片读写、重组器
│   │   │   ├── AuxList.h       # MessageAux 序列化 / 反序列化（避开 Windows AUX 保留名）
│   │   │   ├── NSObject.h      # NSObject 变体类型
│   │   │   ├── KeyedArchiver.h # NSKeyedArchiver/Unarchiver
│   │   │   ├── Transport.h     # 抽象传输（read/write/peek_bytes_ready）
│   │   │   ├── Connection.h    # 客户端：channel 注册、同步/异步 RPC
│   │   │   ├── Server.h        # 服务端：监听器 + Channel 调度
│   │   │   ├── Service.h       # IService 接口 + ServiceDesc
│   │   │   ├── CodegenSupport.h # ArgReader / ResultWriter / DispatchMap
│   │   │   └── Errors.h        # llvm::Error 子类 + ErrorInfo
│   │   ├── lib/
│   │   │   ├── Protocol.cpp
│   │   │   ├── Fragment.cpp
│   │   │   ├── AuxList.cpp     # 避开 Windows AUX 保留名
│   │   │   ├── NSObject.cpp
│   │   │   ├── KeyedArchiver.cpp
│   │   │   ├── Connection.cpp
│   │   │   ├── Server.cpp
│   │   │   ├── Channel.cpp
│   │   │   └── Transport/
│   │   │       ├── TcpTransport.cpp
│   │   │       └── NamedPipeTransport.cpp     # Windows
│   │   └── unittests/
│   │       ├── ProtocolTest.cpp
│   │       ├── AuxRoundTripTest.cpp
│   │       ├── KeyedArchiverTest.cpp
│   │       ├── FragmentReassemblyTest.cpp
│   │       ├── ConnectionMockTest.cpp
│   │       └── CodegenTest.cpp                # 生成 C++ 代理 + Mock 验证
│   │
│   ├── cpp-qt/                 # Qt C++ client runtime —— 独立 Qt 工程
│   │   ├── CMakeLists.txt      # qt6_add_library，可被 Qt Creator 直接打开
│   │   ├── DtxQt.pro           # 可选 qmake 入口
│   │   ├── include/DtxQt/
│   │   │   ├── DtxConnection.h     # 基于 QIODevice / QTcpSocket
│   │   │   ├── DtxFragment.h
│   │   │   ├── DtxAux.h
│   │   │   ├── DtxNSObject.h       # QVariant / QHash 风格
│   │   │   ├── DtxKeyedArchiver.h
│   │   │   ├── DtxTransport.h      # 抽象基类 + QTcpSocket / QLocalSocket 实现
│   │   │   ├── DtxError.h          # 错误码 + QString message
│   │   │   └── DtxCodegenSupport.h # QFuture / 信号槽 glue
│   │   ├── src/
│   │   │   ├── DtxConnection.cpp
│   │   │   ├── DtxFragment.cpp
│   │   │   ├── DtxAux.cpp
│   │   │   ├── DtxNSObject.cpp
│   │   │   ├── DtxKeyedArchiver.cpp
│   │   │   ├── DtxTransport.cpp
│   │   │   └── transport/
│   │   │       ├── DtxTcpTransport.cpp
│   │   │       └── DtxLocalSocketTransport.cpp
│   │   ├── examples/
│   │   │   └── dtx-qt-probe/   # QML + Widgets 示例，列出 published capabilities
│   │   └── tests/
│   │       ├── tst_protocol.cpp
│   │       ├── tst_aux_roundtrip.cpp
│   │       ├── tst_keyed_archiver.cpp
│   │       └── tst_connection_mock.cpp
│   │
│   ├── kotlin/                 # Kotlin / JVM client runtime —— Gradle 工程
│   │   ├── settings.gradle.kts
│   │   ├── build.gradle.kts                    # JVM target；不引入 Android 依赖
│   │   ├── README.md
│   │   ├── src/main/kotlin/org/llvm/dtx/
│   │   │   ├── Protocol.kt                     # MessageHeader + flags
│   │   │   ├── Fragment.kt
│   │   │   ├── Aux.kt
│   │   │   ├── NSObject.kt                     # sealed class 变体
│   │   │   ├── KeyedArchiver.kt
│   │   │   ├── Transport.kt                    # 接口 + TcpTransport 默认实现
│   │   │   ├── Connection.kt                   # suspend 风格 client
│   │   │   ├── DtxError.kt
│   │   │   └── codegen/
│   │   │       ├── ArgReader.kt
│   │   │       ├── ArgWriter.kt
│   │   │       └── NsCodec.kt
│   │   └── src/test/kotlin/org/llvm/dtx/
│   │       ├── ProtocolTest.kt
│   │       ├── AuxRoundTripTest.kt
│   │       ├── KeyedArchiverTest.kt
│   │       └── ConnectionMockTest.kt
│   │
│   └── csharp/                 # .NET client runtime —— 独立 dotnet solution
│       ├── Dtx.sln
│       ├── README.md
│       ├── src/Dtx/
│       │   ├── Dtx.csproj                      # net8.0；nullable enable
│       │   ├── Protocol.cs
│       │   ├── Fragment.cs
│       │   ├── AuxList.cs
│       │   ├── NSObject.cs                     # record + DU 风格
│       │   ├── KeyedArchiver.cs
│       │   ├── Transport/
│       │   │   ├── ITransport.cs
│       │   │   ├── TcpTransport.cs
│       │   │   └── NamedPipeTransport.cs       # Windows local socket
│       │   ├── Connection.cs                   # Task<T> 风格 client
│       │   ├── DtxError.cs
│       │   └── Codegen/
│       │       ├── ArgReader.cs
│       │       ├── ArgWriter.cs
│       │       └── INsCodec.cs
│       └── tests/Dtx.Tests/
│           ├── Dtx.Tests.csproj                # xunit
│           ├── ProtocolTests.cs
│           ├── AuxRoundTripTests.cs
│           ├── KeyedArchiverTests.cs
│           └── ConnectionMockTests.cs
│
├── tools/                      # CLI 工具 —— 默认链接 cpp-llvm runtime
│   ├── dtx-probe/              # 列出 published capabilities / channels
│   ├── dtx-bridge/             # 在两个 Transport 之间中继 DTX 流量（调试用）
│   └── dtx-codegen/            # TableGen 驱动的多语言 codegen
│       ├── CMakeLists.txt
│       ├── Schema.{h,cpp}      # .td → 中间模型
│       ├── Backend.h
│       └── backends/
│           ├── CppLlvmBackend.cpp
│           ├── CppQtBackend.cpp
│           ├── KotlinBackend.cpp
│           └── CSharpBackend.cpp
│
└── test/                       # 跨语言集成测试入口
    ├── lit/                    # llvm-lit + dtx-probe / dtx-bridge 录制回放
    ├── codegen/                # FileCheck 校验 LLVM C++ / Qt C++ / Kotlin / C# 输出
    └── interop/                # 跨 runtime 互通脚本（cpp-llvm ↔ kotlin ↔ csharp）
```

构建集成步骤：

1. `llvm/CMakeLists.txt` 中把 `dtx` 注册为可选子项目（`LLVM_ENABLE_PROJECTS=dtx`）。
   该开关只控制 `runtime/cpp-llvm/`、`tools/`、`test/lit`、`test/codegen` 的纳入。
2. `runtime/cpp-qt/`、`runtime/kotlin/`、`runtime/csharp/` 各自有独立构建系统
   （Qt CMake / Gradle / dotnet），LLVM 主构建**不递归构建**它们；只有专用 CI
   job 才进入对应目录。这样保持 `ninja check-llvm` 不引入 Qt / JDK / .NET 依赖。
3. LLVM 公共依赖：`LLVMSupport`（Endian / Error / StringRef / SmallVector /
   JSON 用于 plist xml 调试）、`LLVMBinaryFormat`（必要时的 leb128 等）。
4. 第三方：`libplist` 不引入；各语言 runtime 各自实现 `bplist00` 子集（仅
   覆盖 NSKeyedArchive 实际用到的对象图，详见 §5）。
5. 单测：cpp-llvm 用 `add_llvm_unittest`（gtest）；cpp-qt 用 Qt Test；kotlin
   用 JUnit5；csharp 用 xUnit。所有 runtime 跑 §6 中相同的 fixture 集合。
6. `dtx::dtx` interface target 仅由 cpp-llvm runtime 暴露，供 DebugHost 进程
   及其内嵌组件（含 lldb / clang-tools-extra）链接；其它 runtime 通过包管理 /
   SDK 形式分发给 IDE 插件工程。
7. `dtx-codegen` 作为 LLVM host tool 构建：LLVM C++ 产物可直接进入 LLVM build
   graph；Qt / Kotlin / C# 产物默认写入显式 `--out-dir`，由各自工程消费。
8. `fixtures/` 走 git LFS；schema / fixtures 的版本在 `runtime/*/README.md`
   中以 commit hash 形式声明，避免某个 runtime 自行复制后漂移。

---

## 4. 公共 API 草图

```cpp
namespace llvm::dtx {

struct MessageHeader {
  uint32_t Magic;            // 0x1F3D5B79
  uint32_t HeaderLength;     // 0x20
  uint16_t FragmentId;
  uint16_t FragmentCount;
  uint32_t PayloadLength;
  uint32_t MessageId;
  uint32_t ConversationIndex;
  uint32_t ChannelCode;
  uint32_t ExpectsReply;
};
static_assert(sizeof(MessageHeader) == 0x20);

enum class PayloadFlag : uint32_t {
  Empty       = 0x00,
  AsyncObject = 0x01,
  Selector    = 0x02,  // selector + aux
  Reply       = 0x03,  // single object
  Object      = 0x04,
  Null        = 0x05,
};

class Transport {
public:
  virtual ~Transport() = default;
  virtual Expected<size_t> bytesReady() = 0;
  virtual Error read(MutableArrayRef<uint8_t>) = 0;
  virtual Error write(ArrayRef<uint8_t>) = 0;
};

class Connection {
public:
  static Expected<std::unique_ptr<Connection>> create(std::unique_ptr<Transport>);
  Expected<uint32_t> makeChannel(StringRef name);
  Expected<ns::Object> call(uint32_t channel, StringRef selector,
                            ArrayRef<Arg> args, bool expectsReply = true);
  Error registerProcessor(uint32_t channel,
                          unique_function<Error(Message)> processor);
  Error stop();
};

class Server {
public:
  explicit Server(std::unique_ptr<Acceptor>);
  template <class S> void addService();   // 调用 ServiceDesc<S>
  Error serve();
};

} // namespace llvm::dtx
```

错误处理统一走 `llvm::Error` / `llvm::Expected`，禁止异常。

---

## 5. NSKeyedArchiver 子集

DTX 全部高层语义都嵌在 NSKeyedArchive 里，必须落地：

- 解析 `bplist00` 头、trailer、offset table（参考
  `CoreFoundation/CFBinaryPList.c`）。
- 对象类型覆盖：`null`、`bool`、`int64`、`real`、`date`、`data`、`ASCII/UTF16
  string`、`uid`、`array`、`set`、`dict`。
- `NSKeyedArchiver` 上层语义：`$top`、`$objects`、`$archiver=NSKeyedArchiver`、
  `$version=100000`、`CF$UID` 解引用。
- 类层级（按调试场景实际需要选取，不做完整 Cocoa 兼容）：
  - `NSString` / `NSMutableString`
  - `NSNumber`（整数 / 浮点 / 布尔）
  - `NSData`（用于内存 dump、变量原始字节）
  - `NSArray` / `NSMutableArray` / `NSSet`
  - `NSDictionary` / `NSMutableDictionary`
  - `NSDate`（事件时间戳）
  - `NSError`（`domain` / `code` / `userInfo`，承载 DebugHost 错误）

测试 fixtures 复用 IrisBuild 的 Rust 实现录制流量
[test_data](../../IrisBuild/rust-crates/dtx/test_data/)，对其按字节做
round-trip 校验，确保 NSKeyedArchive 实现与 Rust 端互通。后续 DebugHost ↔
IDE 自有的服务录制流量另存于 `fixtures/frames/`。

非目标：完整 NSCoder 兼容；只做 DTX 流量里出现过的类型。

---

## 6. DTX 协议代码生成器设计

Rust 侧通过 `#[service]` proc-macro 把 trait 自动展开为 client agent、
processor trait、`ServiceDesc`、selector dispatch 表，以及参数打包/解包逻辑
（见 [dtx_macros](../../IrisBuild/rust-crates/dtx/dtx_macros/)）。llvm-project
侧不应把这些样板在 C++、Kotlin、C# 各写一遍，而是以一个 DTX IDL 作为唯一
服务定义源，由 `dtx-codegen` 生成多语言薄封装。

**生成范围按语言划分**：

| 语言        | Client 代理 | Server / Dispatch | 备注                                         |
| ----------- | :---------: | :---------------: | -------------------------------------------- |
| C++ (LLVM)  | ✅          | ✅                | llvm-project 内主后端，需要双向能力          |
| C++ (Qt)    | ✅          | ❌                | 面向 Qt/QML 工具与 IDE 集成，仅 client       |
| Kotlin      | ✅          | ❌                | 仅生成 client，外部 JVM/Android 工具消费     |
| C#          | ✅          | ❌                | 仅生成 client，外部 .NET / Windows 工具消费  |

Qt / Kotlin / C# 后端只面向 IDE 客户端场景（调用 DebugHost 暴露的服务），
不生成 `Processor` / `Dispatcher` / `ServiceDesc`。若未来确有 Qt/JVM/.NET 侧
承担 DebugHost 角色的需求，再单独评估。生成器在所有语言下都只生成代理和
dispatch glue（仅 LLVM C++）；帧格式、Aux、NSKeyedArchiver、Transport 仍由
各语言 runtime 库实现。

注意 LLVM C++ 与 Qt C++ 是两套独立的 C++ 后端：前者依赖 `llvm::Error` /
`llvm::Expected` / `llvm::SmallVector` 等 LLVM 设施，后者依赖 Qt 容器、
信号槽与 `QFuture`，两者不共享生成代码，避免把 LLVM 头文件强行拉进 Qt 工程。

### 6.1 输入模型

首选 LLVM TableGen 作为 IDL 载体，原因是它可直接接入 LLVM CMake、lit、
FileCheck 和 `llvm-tblgen` 风格的后端。`.td` 文件只表达 DTX RPC 语义，不表达
Transport 实现：

```tablegen
include "DTXBase.td"

def ProcessControl : DTXService<
  "ProcessControl",
  "org.llvm.debughost.process",
  1
> {
  let Methods = [
    DTXMethod<"listProcesses", [], DTXArray<DTXObject<"ProcessInfo">>>,
    DTXMethod<"attachToPID",
      [
        DTXArg<"pid", DTXInt32>,
        DTXArg<"options", DTXObject<"AttachOptions">>
      ],
      DTXObject<"AttachResult">,
      "attachToPID:options:"
    >
  ];
}
```

IDL 必须覆盖以下字段：

- `service`：生成名、DTX channel identifier、capability version、可选 package /
  namespace 覆盖。channel identifier 由 DebugHost 与 IDE 双方约定，本项目
  使用 `org.llvm.debughost.*` 命名空间，与历史上 Apple 的 `com.apple.*`
  服务标识互斥。
- `method`：本地方法名、selector、参数列表、返回类型、调用方向（IDE → host
  call、host → IDE callback / event、notification）、是否期望 reply。
- `type`：`void`、`bool`、`i32/u32/i64/u64`、`double`、`string`、`data`、
  `object`、`array<T>`、`dict<K,V>`、`nullable<T>`、以及按 NSKeyedArchive 字典
  编码的命名对象。
- `selector` 推导规则默认对齐 Rust 宏：0 参数为 lowerCamel 方法名；1 参数为
  `method:`；多参数为 `method:firstArgRest:` 形式。需要与外部既有 selector
  对齐时必须在 IDL 中显式写出，避免跨语言生成器各自猜测。

### 6.2 生成器架构

`tools/dtx-codegen/` 使用 LLVM TableGen parser 构建语言无关的中间模型：

```text
.td -> RecordKeeper -> DTXSchema(Service/Method/Type) -> Backend
                                                   ├── CppLlvmBackend  (DebugHost + cpp-llvm IDE 测试)
                                                   ├── CppQtBackend    (Qt IDE 客户端)
                                                   ├── KotlinBackend   (JetBrains 平台 IDE)
                                                   └── CSharpBackend   (Visual Studio / .NET IDE)
```

前端校验在生成前完成，错误需要带 `.td` 源位置：

- service channel 不可为空，version 必须非 0。
- 同一 service 内 selector 不可重复。
- 参数名必须稳定，因为多参数 selector 推导依赖参数名。
- 类型必须能映射到四种语言；不能映射的类型必须声明为 `DTXObject` 或
  `DTXRawObject`，由业务层手动解码。
- 返回值只能有 0 或 1 个；多值返回统一建模为命名对象或 tuple-like 对象。

命令行接口：

```text
dtx-codegen --input schema/DebugHostServices.td --lang=cpp    --out-dir <dir>
dtx-codegen --input schema/DebugHostServices.td --lang=qt     --namespace dtx::qt    --out-dir <dir>
dtx-codegen --input schema/DebugHostServices.td --lang=kotlin --package org.llvm.dtx --out-dir <dir>
dtx-codegen --input schema/DebugHostServices.td --lang=csharp --namespace LLVM.DTX   --out-dir <dir>
```

生成器输出必须稳定排序，禁止写时间戳，便于 review 和 FileCheck。

### 6.3 LLVM C++ 后端（DebugHost + 测试用 client）

LLVM C++ 是 llvm-project 内的主后端，**同时生成 client 与 server**：DebugHost
进程使用 server 端 dispatch；CLI 工具与单测复用 client 端代理验证 dispatch。
生成物贴合 §4 的 `Connection` / `Server` API，使用 `llvm::Error` /
`llvm::Expected`：

- `<Service>Client.h.inc`：生成强类型 client proxy。每个方法负责构造
  `AuxList`、调用 `Connection::call` / `callVoid`，并把 `ns::Object` 转回强类型。
- `<Service>Service.h.inc`：生成 `I<Service>Processor` 抽象接口、
  `ServiceDesc<<Service>Service<P>>`、selector dispatch 表。DebugHost 实现
  `I<Service>Processor`。
- `<Service>Types.h.inc`：生成命名对象的轻量 struct，以及 `fromNSObject` /
  `toNSObject` 声明；复杂对象的实现可选择生成或手写特化。
- `CodegenSupport.h` 提供 `ArgReader`、`ArgWriter`、`ResultWriter`、
  `DispatchMap`，生成代码只调用这些稳定小 API，避免散落手写解包逻辑。

示意：

```cpp
class ProcessControlClient {
public:
  Expected<SmallVector<ProcessInfo>> listProcesses();
  Expected<AttachResult> attachToPID(int32_t Pid, AttachOptions Opts);
};

class IProcessControlProcessor {
public:
  virtual ~IProcessControlProcessor() = default;
  virtual Expected<SmallVector<ProcessInfo>> listProcesses() = 0;
  virtual Expected<AttachResult> attachToPID(int32_t Pid, AttachOptions Opts) = 0;
};
```

### 6.4 Qt C++ 后端（client-only）

Qt 后端面向 Qt Widgets / QML / Qt Creator 等 Windows / macOS / Linux 工具，
**只生成 client 代理**。runtime 在外部 Qt 工程提供，约定包含
`DtxConnection`（基于 `QIODevice` 或自有 socket）、`DtxAuxWriter`、
`DtxArgReader`、`NSObject` 与 `NsCodec<T>`：

- `<Service>Client.h` / `<Service>Client.cpp`：生成 `class <Service>Client :
  public QObject`，方法返回 `QFuture<T>`，并对每个 RPC 暴露
  `<method>Finished(...)` / `<method>Failed(QString)` 信号，便于 UI 直连。
- `<Service>Types.h`：生成 `Q_GADGET` 风格 POD（属性 + `Q_PROPERTY`），命名
  NSKeyedArchive 对象作为可被 QML 暴露的值类型；通过 `Q_DECLARE_METATYPE`
  注册到 Qt meta 系统。
- 容器类型映射到 Qt 容器：`array<T>` → `QList<T>`，`dict<K,V>` →
  `QHash<K,V>`，`string` → `QString`，`data` → `QByteArray`，`nullable<T>` →
  `std::optional<T>`（Qt 6）或 `QPointer` 风格包装（Qt 5，仅当目标 runtime
  显式声明 Qt 5 兼容时启用）。
- 错误统一通过 `QFuture` 失败态 + `<method>Failed` 信号承载，绝不抛 C++ 异常。
- 不依赖任何 LLVM 头文件；生成器输出的 `.h` / `.cpp` 通过普通 `qmake` /
  `CMake` + `qt6_add_library` 即可消费。
- 不生成 `Processor` / `Dispatcher`；server-only 方向的方法被忽略并记录日志。

### 6.5 Kotlin 后端（client-only）

Kotlin 后端面向 JVM / Android 工具侧，不生成 Android framework 依赖，**只生成
client 代理**。runtime 约定提供 `DtxConnection`、`DtxAuxWriter`、
`DtxArgReader`、`NSObject` 和 `NsCodec<T>`：

- `<Service>Client.kt`：生成 `class <Service>Client(private val connection:
  DtxConnection)`，方法默认生成 `suspend fun`，通过 runtime 决定阻塞或协程调度。
- `<Service>Types.kt`：生成 `data class` 与 `NsCodec` 骨架；未知 NSObject 类型
  保留为 `NSObject`，避免生成器强行猜字段。
- 不生成 `Processor` / `Dispatcher` / `ServiceDesc`。schema 中的 server-only
  callback 方向方法在 Kotlin 侧被忽略并写入生成日志，便于排查。

类型映射：

| IDL 类型 | Kotlin |
| -------- | ------ |
| `bool` | `Boolean` |
| `i32/u32` | `Int`（`u32` 编解码时按无符号位模式处理） |
| `i64/u64` | `Long`（`u64` 编解码时按无符号位模式处理） |
| `double` | `Double` |
| `string` | `String` |
| `data` | `ByteArray` |
| `array<T>` | `List<T>` |
| `dict<K,V>` | `Map<K, V>` |
| `nullable<T>` | `T?` |

### 6.6 C# 后端（client-only）

C# 后端面向 .NET 工具、Windows 主机和可能的 Unity/IDE 集成，**只生成 client
代理**。runtime 约定提供 `IDtxConnection`、`DtxAuxWriter`、`DtxArgReader`、
`NSObject` 和 `INsCodec<T>`：

- `<Service>Client.g.cs`：生成 `partial class <Service>Client`，同步 DTX RPC
  以 `Task<T>` / `Task` 暴露，支持 `CancellationToken` 参数。
- `<Service>Types.g.cs`：生成 `partial record` 或 `partial struct`；复杂对象允许
  用户在另一个 partial 文件里补 `INsCodec<T>` 特化。
- 不生成 `IProcessor` / `Dispatcher`；server-only 方向的方法被忽略并记录日志。

类型映射：

| IDL 类型 | C# |
| -------- | -- |
| `bool` | `bool` |
| `i32/u32` | `int` / `uint` |
| `i64/u64` | `long` / `ulong` |
| `double` | `double` |
| `string` | `string` |
| `data` | `byte[]` 或 `ReadOnlyMemory<byte>`（由 `--csharp-data-type` 控制） |
| `array<T>` | `IReadOnlyList<T>` |
| `dict<K,V>` | `IReadOnlyDictionary<K, V>` |
| `nullable<T>` | `T?` |

### 6.7 生成边界与版本策略

- 生成器不生成 Transport、分片重组、bplist parser、线程模型或设备发现逻辑。
- 生成代码不得吞掉线协议错误；参数个数、类型不匹配、selector 未命中都要回到
  runtime 的结构化错误。
- 每个生成文件头部写入 schema 文件名和 service 名，不写绝对路径。
- schema 变更必须先通过 C++ 后端编译测试，再通过 Kotlin / C# FileCheck golden。
- 若同一 service 在不同 DebugHost 版本间出现 selector 差异，在 IDL 中以
  `availability` 标注，生成器输出运行时 capability/version 检查，而不是分叉
  service 名。
- Qt / Kotlin / C# 仅 client：schema 中标记为 server callback 的方法不会进入
  这三种语言的输出。这是规划层显式约束，不是生成器 bug，需要通过 IDL 注释和
  生成日志让使用者明确感知。

---

## 7. Transport 后端

DebugHost ↔ IDE 是同机或局域网通路，不涉及 USB / 设备总线。Transport 层目标
平台支持 Windows / macOS / Linux。

| 后端                | 介质                                                          | 主要场景                                                     |
| ------------------- | ------------------------------------------------------------- | ------------------------------------------------------------ |
| TCP                 | `AF_INET` / `AF_INET6` loopback 或 LAN                         | 默认通路；DebugHost 与 IDE 在同机或同子网                    |
| Unix Domain Socket  | POSIX `AF_UNIX`                                                | macOS / Linux 同机首选，权限和路径走标准 runtime 目录        |
| Windows Named Pipe  | `\\.\pipe\llvm-debughost-*`                                    | Windows 同机首选，避免占用 TCP 端口                          |
| Mock (in-mem)       | 双向 ring buffer                                               | 仅供单测；用 `MockTransport` 驱动 client / server 双向 round-trip |

Transport 与协议解耦：协议层只调用 `read/write/bytesReady`。每种语言 runtime
至少实现 TCP；Unix Domain Socket / Named Pipe 在对应平台 runtime 中作为可选
后端提供。

不在本计划的 Transport 范围内：USB（usbmuxd 等）、iOS RemoteXPC / tunneld、
任何蓝牙 / IPCC 私有传输。这类传输如果要接入，应由 DebugHost 内部桥接到上述
四种通路之一，而不是把异构传输塞进协议层。

---

## 8. 并发模型

- 客户端 `Connection`：单 IO 线程（参考 Rust `IoDispatcher`），主线程通过
  无锁队列投递 `RpcCall`，IO 线程负责拼帧 + 分发。
- 同步 RPC：通过 `std::promise` / `llvm::ThreadPool` 任务句柄等待。
- 异步 channel：每 channel 一个轻量任务（`llvm::ThreadPool::async`），
  内部维护 `ReorderedStream` 处理乱序分片。
- 服务端 `Server`（DebugHost 侧）：每个 IDE 连接一个调度线程；channel handler
  复用 ThreadPool。事件 channel（如 stop event、output stream）以异步消息形式
  推送给所有订阅 IDE。
- 不引入第三方 async 运行时，全部基于 `llvm/Support/ThreadPool.h` 与
  `std::condition_variable`，便于嵌入 DebugHost 进程。

---

## 9. DebugHost 集成

DTX 在本项目中是 DebugHost 的**北向接口**。DebugHost 内部如何驱动 LLDB /
mixdevice / 其它后端，不在本协议规划范围。

集成约束：

1. DebugHost 进程链接 `dtx::dtx`（cpp-llvm runtime），实现一组 `IService`
   processor（process control、threads、breakpoints、expressions、events 等）。
2. 启动时从配置中选择 Transport（TCP / UDS / Named Pipe），监听后等待 IDE
   连接；每个 IDE 连接独立 session，互不可见对方的私有 channel。
3. 鉴权与多路复用：本计划不规定具体鉴权方案，DebugHost 可在握手 channel 上
   发布 capabilities 与 token 校验流程；IDE 客户端 runtime 必须把握手错误以
   结构化错误形式返回，禁止吞掉。
4. IDE 端引用各自语言的 client runtime + codegen 产物，不直接持有 LLDB / 调试
   后端的句柄。所有跨进程状态都走 DTX。
5. DebugHost 内部若需要嵌入 LLDB 自身的 gdb-remote 通路，作为内部实现细节，
   不暴露给 IDE。

---

## 10. 测试策略

- **单元**（gtest）：
  - 头部 endian / 边界（`MessageHeader::magic` / `HeaderLength` 错误返回 Error）。
  - Aux 解包/打包：用 `data + 1 obj + 1 i32 + 1 u64 + nil` 组合 round-trip。
  - Fragment 重组：覆盖单帧、首帧、乱序、超时。
  - KeyedArchiver：复用 IrisBuild 的四个 bplist 做字节级 round-trip。
  - Codegen C++：用最小 `.td` 生成 client / service，编进 gtest，通过
    `MockTransport` 验证 selector、参数顺序、reply 包装与手写路径一致。
- **集成**（lit + FileCheck）：
  - `dtx-probe --record golden.bin` → 与 IrisBuild Rust 实现互发互收的离线
    流量做 diff，验证线协议字节级一致。
  - 录制 / 回放 mock，使 CI 在无 DebugHost 真实后端的情况下亦可跑。
  - `dtx-codegen` 对同一 schema 分别输出 LLVM C++ / Qt C++ / Kotlin / C#，用
    FileCheck 校验 selector、类型映射、错误分支和稳定排序；Qt / Kotlin / C#
    默认只做文本 golden，有对应 SDK 的构建机再启用编译 smoke test（Qt 后端的
    smoke test 链接最小 Qt Core / Qt Network 即可）。
- **互操作**：
  - 启动 cpp-llvm `Server`（最小 DebugHost 桩），用 cpp-qt / kotlin / csharp
    client 分别连接，跑同一组 schema 调用并比较返回。
  - 与 IrisBuild Rust 实现互测，确认线协议常量未漂移。
- **静态检查**：开启 `-Wall -Wextra -Wconversion`，必要时 `clang-tidy
  bugprone-*`。

---

## 11. 里程碑

| 阶段 | 内容                                                                  | 退出标准                                                |
| ---- | --------------------------------------------------------------------- | ------------------------------------------------------- |
| M0   | `runtime/cpp-llvm/` 脚手架、CMake 集成、`MessageHeader` POD + endian 解析、`schema/` + `fixtures/` 入仓 | `ninja dtx-unittests` 跑通 ProtocolTest                 |
| M1   | cpp-llvm bplist 解析器 + `NSObject` 变体 + KeyedArchiver round-trip   | 四个 fixture 字节相等                                   |
| M2   | cpp-llvm `AuxList`、`Fragment` 收发、`Connection::makeChannel` + 同步 `call` | 与 Rust 端跑通 capability 握手 + 一次同步 RPC          |
| M3   | cpp-llvm 异步 channel + 乱序重组 + 手写 Server 侧 dispatch 框架        | 最小 DebugHost 桩与 cpp-llvm client 互通                |
| M4   | TCP / UDS / Named Pipe Transport 三平台落地 + `dtx-probe` / `dtx-bridge` | 三种 Transport 在 Win/macOS/Linux CI 上跑通 echo 服务   |
| M5   | `dtx-codegen` LLVM C++ 后端 + TableGen 服务定义，迁移手写 dispatch 表  | 生成 LLVM C++ 产物编进 unittests，行为与 M3 手写 dispatch 一致 |
| M6   | `runtime/cpp-qt/` + `runtime/kotlin/` + `runtime/csharp/` 基础 runtime（手写：协议、Aux、KeyedArchiver、Connection、TCP transport） | 三套 runtime 各自单测跑通 fixture，能用手写 client 调通本地 DebugHost 桩 |
| M7   | `dtx-codegen` Qt / Kotlin / C# **client-only** 后端 + golden 测试；首批 IDE 插件接入 | 三种 client 产物 golden 稳定，至少一个真实 IDE 插件用生成代码连接 DebugHost 成功 |

---

## 12. 已知风险与待研究项

- **bplist 实现成本**：自实现需对齐 CoreFoundation 行为；前期可用 `JSON
  fallback` 仅做 dump，重点保证 NSKeyedArchive 流程。
- **压缩 flag**：Rust 侧 `compression_flags != 0` 直接 `todo!`。本协议在
  DebugHost ↔ IDE 链路上默认不启用压缩，但保留 `(flags & 0xFF000) >> 12` 字段
  以便和 Rust 实现互测。若后续需要压大对象，可启用 LZ4，对应 `llvm/Support/
  Compression.h` 适配。
- **跨平台 socket 等待**：`bytes_ready` 在 Windows 用 `ioctlsocket(FIONREAD)`，
  Linux / macOS 用 `ioctl(FIONREAD)`，统一封到 Transport 实现里。
- **跨语言 runtime 漂移**：Qt / Kotlin / C# 生成物依赖各自 runtime 的 Aux 与
  NSObject 编解码实现。schema 和 golden 只能保证 API 与 selector 形状，仍需用
  共享抓包 fixtures 做字节级互测。
- **TableGen 表达能力边界**：TableGen 适合声明式服务描述，但不适合复杂对象转换
  逻辑。命名对象先生成 codec 骨架，复杂字段由手写特化补齐，避免 IDL 演化成
  半个序列化语言。
- **鉴权与多客户端**：DebugHost 同时面对多个 IDE 时的会话隔离、token 刷新、
  权限分级目前只列出框架（§9 第 3 条），具体方案需在握手 channel 设计阶段
  收敛。

---

## 13. 不在本计划范围

- 与 iOS / macOS 设备直接通信：包括 usbmuxd、lockdown、RemoteXPC、tunneld、
  `com.apple.instruments.*` / `com.apple.dt.*` 等私有服务，以及 DTTap 与
  Instruments `.trace` 文件格式。这些由 DebugHost 的设备后端模块独立处理，
  不通过本协议暴露给 IDE。
- LLDB 内部协议：DebugHost 内部如何驱动 LLDB / gdb-remote 是实现细节，
  不在本规划。
- 与 `lld` / `clang` 的耦合：本子项目不参与编译产物链路，仅复用 monorepo
  构建/测试基础设施。
- 通用 DAP（Debug Adapter Protocol）适配：如果未来需要桥接 DAP，应在 IDE
  侧或 DebugHost 侧单独引入适配层，本协议不为 DAP 让步设计。
