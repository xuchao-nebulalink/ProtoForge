# 协议插件开发指南

一个协议插件只需提供三样东西：**编解码器（framing）**、**消息类型 + 处理器（semantics）**、**配置 Schema**。
它不接触传输层、UI 或设备模型的实现，因此同一个插件二进制既能驱动模拟设备（从站），也能作为测试主站，
并且在 TCP / UDP / 串口 / 内存回环上都能跑。

新增一个协议**不需要修改框架任何一行代码**。

---

## 1. 三层职责边界

| 你要写的 | 它负责 | 它不负责 |
| --- | --- | --- |
| `IFrameCodec` | 帧头、帧长、校验、分帧、重同步 | 报文含义 |
| `MessageBase<T>` 派生类 | 报文体的编解码 | 帧头、校验、地址 |
| Handler 类 | 业务语义、读写设备 | 字节序列化 |

这个切分的实际收益：Modbus RTU 和 Modbus TCP 功能码完全相同、组帧完全不同，
所以两者共用同一套消息类型和 Handler，只换 codec。

---

## 2. 最小插件骨架

目录：`plugins/<yourprotocol>/`

```
plugins/myproto/
├── CMakeLists.txt
├── myproto.json          # Qt 插件元数据
├── MyProtoPlugin.h/.cpp  # IProtocolPlugin 实现
├── MyProtoCodec.h/.cpp   # IFrameCodec 实现
├── MyProtoMessages.h     # 消息类型
└── MyProtoHandlers.h/.cpp
```

### 2.1 定义消息类型

消息用 CRTP 基类 `MessageBase<T>`，它负责生成 `messageType()`。你需要提供三件事：

```cpp
#include <protocol/IMessage.h>

using namespace hwsim;

struct ReadTemperature : protocol::MessageBase<ReadTemperature> {
    quint16 channel{0};

    // 1) 这个报文在线上的操作码
    static constexpr protocol::OpCode opcode() { return 0x21; }

    // 2) 从帧体解码
    static core::Result<ReadTemperature> decode(const protocol::Frame& frame)
    {
        if (frame.payload.size() < 2) {
            return core::makeError(core::ErrorCode::FrameMalformed,
                                   QStringLiteral("需要 2 字节通道号"));
        }
        ReadTemperature message;
        message.channel = core::endian::readBig<quint16>(core::hex::asBytes(frame.payload));
        return message;
    }

    // 3) 编码回帧体（不含帧头和校验，那是 codec 的事）
    core::Result<QByteArray> encodeBody(const protocol::EncodeContext&) const
    {
        std::array<std::byte, 2> raw{};
        core::endian::writeBig<quint16>(raw, channel);
        return core::hex::toByteArray(raw);
    }

    QString describe() const override
    {
        return QStringLiteral("ReadTemperature ch=%1").arg(channel);
    }
};
```

这三个成员由 concept 校验（`DecodableMessage` / `EncodableMessage`）。签名写错会在**注册那一行**报错，
而不是在模板实例化的深处。

### 2.2 写 Handler

Handler 是普通类，只需要一个 `handle` 方法。不需要继承任何东西：

```cpp
class ReadTemperatureHandler {
public:
    core::Result<protocol::MessagePtr> handle(const ReadTemperature& request,
                                              protocol::ExecutionContext& context)
    {
        const auto value = context.read(0x1000 + request.channel);
        if (value.hasError()) {
            auto error = std::make_shared<ErrorResponse>();
            error->code = 0x02;   // 非法数据地址
            return protocol::MessagePtr(error);
        }

        auto reply = std::make_shared<TemperatureReport>();
        reply->channel = request.channel;
        reply->celsius = value.value().toDouble();
        return protocol::MessagePtr(reply);
    }
};
```

要点：

- 返回 `MessagePtr{}`（空指针）表示**不回复**，用于广播帧或只写命令。
- 返回 `Error` 表示处理失败，会被记入日志和统计，不会自动回复。要回协议异常码，
  就像上面那样返回一个异常响应报文。
- `ExecutionContext` 提供 `read/write/readRange/writeRange`，以及 `device()` 拿到完整
  `IDeviceAccess`。用 `requireDevice()` 代替裸判空。

### 2.3 实现 Codec

```cpp
class MyProtoCodec final : public protocol::IFrameCodec {
public:
    QString name() const override { return QStringLiteral("myproto"); }

    protocol::FrameScanResult scan(std::span<const std::byte> buffer,
                                   transport::Direction) const override
    {
        if (buffer.size() < kHeaderSize) {
            return protocol::FrameScanResult::needMoreData();
        }
        if (std::to_integer<quint8>(buffer[0]) != kStartByte) {
            // 丢一个字节重新同步，而不是报错卡死
            return protocol::FrameScanResult::discard(1, QStringLiteral("帧头错误"));
        }
        ...
        return protocol::FrameScanResult::ready(std::move(frame), totalLength);
    }

    core::Result<QByteArray> wrap(protocol::OpCode opcode, const QByteArray& body,
                                  const protocol::EncodeContext& context) const override
    {
        ...
    }
};
```

`scan()` 的三种返回值决定了会话怎么走：

- `NeedMoreData` — 等下一次读，缓冲区不动
- `FrameReady` — 消费 `consumed` 字节并派发 `frame`
- `Discard` — 消费 `consumed` 字节后重新扫描

**用 `Discard` 而不是返回错误**，是串口线噪能自愈的关键。返回 `Discard` 且 `consumed == 0`
会被会话强制按 1 字节处理，防止死循环。

需要请求-响应关联（如 Modbus TCP 的事务 ID）时，实现这两个方法：

```cpp
QString prepareRequest(protocol::EncodeContext& context) const override;  // 主站发请求时分配 token
QString correlationKey(const protocol::Frame& frame) const override;      // 收到响应时提取 token
```

两者都不实现就退化为「最老的未决请求优先匹配」，这对 Modbus RTU 这类严格串行的协议是正确的。

### 2.4 实现插件入口

```cpp
#include <protocol/IProtocolPlugin.h>

class MyProtoPlugin : public QObject, public hwsim::protocol::IProtocolPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID HWSIM_PROTOCOL_PLUGIN_IID FILE "myproto.json")
    Q_INTERFACES(hwsim::protocol::IProtocolPlugin)
    HWSIM_DECLARE_PROTOCOL_PLUGIN     // 自动实现 abiVersion()

public:
    hwsim::protocol::PluginMetadata metadata() const override
    {
        return {QStringLiteral("myproto"), QStringLiteral("私有协议"),
                QStringLiteral("1.0.0"), QStringLiteral("我司"),
                QStringLiteral("……"), {QStringLiteral("v1")}};
    }

    hwsim::core::ConfigSchema configSchema() const override
    {
        hwsim::core::ConfigSchema schema(QStringLiteral("私有协议"));
        schema.add(hwsim::core::ConfigField::integer(
            QStringLiteral("stationAddress"), QStringLiteral("站号"), 1).range(0, 255));
        return schema;
    }

    hwsim::core::Result<hwsim::protocol::FrameCodecPtr>
    createCodec(const QVariantMap& config) const override
    {
        auto codec = std::make_unique<MyProtoCodec>();
        if (auto configured = codec->configure(config); configured.hasError()) {
            return configured.error();
        }
        return hwsim::protocol::FrameCodecPtr(std::move(codec));
    }

    // 全部注册集中在这里，每个报文一行
    hwsim::core::Result<void> registerCommands(hwsim::protocol::CommandRegistry& registry,
                                               const QVariantMap& config) const override
    {
        Q_UNUSED(config)
        registry.bind<ReadTemperature>(std::make_shared<ReadTemperatureHandler>());
        registry.bind<WriteSetpoint>(std::make_shared<WriteSetpointHandler>());
        registry.bindEncoder<TemperatureReport>();   // 只发不收的报文
        registry.bindEncoder<ErrorResponse>();
        return hwsim::core::success();
    }
};

HWSIM_EXPORT_STATIC_PLUGIN(myproto, MyProtoPlugin)   // 静态链接时才展开
```

`myproto.json`：

```json
{
    "id": "myproto",
    "displayName": "私有协议",
    "version": "1.0.0"
}
```

### 2.5 加进构建

`plugins/myproto/CMakeLists.txt`：

```cmake
hwsim_add_protocol_plugin(
    NAME    myproto
    SOURCES
        MyProtoPlugin.h
        MyProtoPlugin.cpp
        MyProtoCodec.h
        MyProtoCodec.cpp
        MyProtoMessages.h
        MyProtoHandlers.h
        MyProtoHandlers.cpp
        myproto.json
)
```

然后在 `plugins/CMakeLists.txt` 里加 `add_subdirectory(myproto)`。

### 2.6 设备主动上报（可选）

Handler 只在收到帧时才跑，所以「下位机自己定时往上推」这类流量没法用 Handler 表达。
实现 `IUnsolicitedSource`，在 `registerCommands()` 里挂到 registry 上即可 —— 那里正好还
握着 Handler 共用的设备状态：

```cpp
class TelemetrySource final : public protocol::IUnsolicitedSource {
public:
    QString name() const override { return QStringLiteral("myproto.telemetry"); }
    int intervalMs() const override { return 20; }          // 0 = 不轮询
    std::vector<MessagePtr> poll(qint64 nowMs) override {    // 返回空表示这一拍不发
        return {std::make_shared<TelemetryFrame>(state_->sample())};
    }
};

registry.setUnsolicitedSource(std::make_shared<TelemetrySource>(state));
```

会话按 `intervalMs()` 轮询它，把返回的报文走 `send()` 发出去（不登记关联项，正是主动上报
该有的语义）；链路没打开时自动跳过，不会攒一堆过期数据等重连后一起喷出去。
参考 `plugins/sw6/` 的 `Sw6StreamSource`，它推的是 0x81 实时位姿流。

---

## 3. 动态还是静态

同一份源码两种链接方式，由 CMake 决定，源码不用改：

```bash
# 全部编成 DLL，运行时从 bin/plugins 扫描加载（默认）
cmake --preset msvc2022 -DHWSIM_PLUGIN_LINKAGE=DYNAMIC

# 全部链进主程序，单文件分发、调试器可直接单步进插件代码
cmake --preset msvc2022-static-plugins

# 只把某一个插件转成静态
cmake --preset msvc2022 -DHWSIM_PLUGIN_MYPROTO_LINKAGE=STATIC
```

静态模式下，CMake 生成 `StaticPluginImports.cpp`，里面引用
`HWSIM_EXPORT_STATIC_PLUGIN` 展开出来的工厂符号。`PluginManager` 把两类插件放进同一张表，
下游按 id 查找，分辨不出区别。

**开发期建议用静态**（调试友好、无 ABI 问题），**对外分发用动态**（第三方可独立编译）。

---

## 4. ABI 与加载失败

插件加载要过三道检查，任何一道不过都只是记录在 `PluginManager::failures()` 并在 UI 里显示，
不会拖垮主程序：

1. `Q_PLUGIN_METADATA` 的 IID 必须等于 `HWSIM_PROTOCOL_PLUGIN_IID`（在**实例化之前**校验，
   所以不兼容的库不会执行任何代码）
2. `abiVersion()` 必须等于 `HWSIM_PLUGIN_ABI_VERSION`（由宏自动实现，不会写错）
3. `metadata().id` 非空且未被占用

改动 `IProtocolPlugin` 接口形状时，必须同时递增 IID 尾部版本号和 `HWSIM_PLUGIN_ABI_VERSION`。

> 注意：动态插件卸载后，它定义的 `TypeId` 名字字符串就悬空了。`PluginManager` 因此在
> 进程生命周期内不卸载已成功加载的插件。

---

## 5. 为什么没有 switch-case

`CommandRegistry` 用三张哈希表替代了三处本该出现的 switch：

```
opcode        -> decoder    收到的帧变成强类型报文
message type  -> handler    该类型的业务逻辑
message type  -> encoder    发出的报文变回帧体
```

`bind<M>(handler)` 一次性填好三张表。所以加一个报文 = 写一个消息类 + 一个 Handler + 一行注册，
框架侧零改动。`HandlerFor<H, M>` concept 保证 Handler 签名和报文类型对得上，对不上编译期就报错。

派发路径上唯一的运行时开销是两次哈希查找，`TypeId` 用类型名的 FNV-1a 哈希做 key，
跨 DLL 边界稳定（不依赖静态变量地址，那在 Windows 上不保证唯一）。

---

## 6. 测试你的插件

不需要开端口，用 `LoopbackTransport` 把两个端点在内存里对接：

```cpp
LoopbackTransport slaveEndpoint;
LoopbackTransport masterEndpoint;
slaveEndpoint.open(TransportConfig(TransportKind::Loopback));
masterEndpoint.open(TransportConfig(TransportKind::Loopback));
LoopbackTransport::connectPair(&slaveEndpoint, &masterEndpoint);

// 从站侧：插件 + 设备模型
auto registry = std::make_shared<CommandRegistry>();
plugin->registerCommands(*registry, config);
ProtocolSession slave(slaveEndpoint.primaryLink(), plugin->createCodec(config).value(),
                      registry, slaveOptions);
slave.setDevice(&deviceModel);

// 主站侧：同一个插件，Initiator 角色
ProtocolSession master(masterEndpoint.primaryLink(), plugin->createCodec(config).value(),
                       masterRegistry, masterOptions);

master.sendRequest(std::make_shared<ReadTemperature>(), [](Result<MessagePtr> reply) {
    QVERIFY(reply.hasValue());
});
```

参考 `tests/integration/tst_endtoend.cpp`，里面有完整的主从闭环和故障注入断言。

单测 Handler 更简单，塞一个 stub `IDeviceAccess` 进 `ExecutionContext` 即可，
不需要任何传输和编解码，见 `tests/unit/tst_protocol.cpp`。

---

## 7. 检查清单

- [ ] 每个消息类型都有 `opcode()` / `decode()` / `encodeBody()` / `describe()`
- [ ] `describe()` 输出足够定位问题（报文视图和日志都用它）
- [ ] `scan()` 对半包返回 `NeedMoreData`，对噪声返回 `Discard`，永不抛异常
- [ ] `scan()` 能处理一次读到多帧（粘包）
- [ ] 长度字段做了上界检查，避免恶意长度导致巨额分配
- [ ] `registerCommands()` 里每个 opcode 只注册一次（重复会返回 false 并记 warning）
- [ ] 只发不收的报文调用了 `bindEncoder<M>()`
- [ ] 设备主动推的流量走 `IUnsolicitedSource`，不要试图从 Handler 里发
- [ ] 插件本身不含任何故障注入逻辑（那是 `IByteFilter` 的职责）
- [ ] `configSchema()` 覆盖了所有可配项，UI 面板由它自动生成
