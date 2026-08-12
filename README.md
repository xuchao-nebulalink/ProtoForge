# 设备协议模拟平台

Qt 6.5 LTS + C++20 的上位机设备协议模拟平台。双向对等：既能模拟下位机设备（从站）等上位机来连，
也能作为测试主站去驱动真实设备。协议以插件扩展，**新增协议不需要修改框架代码**。

## 能做什么

- **统一通讯层** — TCP 服务端 / TCP 客户端 / UDP / 串口 / 内存回环，一套抽象
- **协议插件** — 动态 DLL 或静态链接，同一份源码，构建期决定
- **零 switch-case 派发** — 三张哈希表替代解码、派发、编码三处 switch
- **设备仿真** — 参数树（名称 + 地址双索引）、可配置状态机、实时数据发生器、故障注入
- **故障模拟** — 丢包、响应超时、CRC 错误、误码、帧截断、插入乱码、附加时延、主动断链
- **图形界面** — 设备树、报文收发视图（十六进制 / 文本 / 解析三视图）、参数面板、日志窗口
- **自动化测试** — 无头模式 + JavaScript 场景脚本，退出码即测试结论

## 环境要求

| 组件 | 版本 |
| --- | --- |
| Qt | 6.5 LTS 或更高，需要 Core / Network / SerialPort / Gui / Widgets / Qml / Test |
| 编译器 | MSVC 2022（推荐）或 MSVC 2019 16.11+ |
| CMake | 3.21 或更高 |

> MSVC 2019 缺少 `<format>` 和部分 `ranges`。`cmake/Cpp20Features.cmake` 会自动探测并回退到
> Qt 原生实现，不影响功能。

## 构建

```powershell
# 指向你的 Qt 安装
$env:QT6_DIR = "C:/Qt/6.5.3/msvc2019_64"

cmake --preset msvc2022
cmake --build --preset msvc2022-debug
ctest --preset msvc2022-debug
```

可用的 preset：

| preset | 说明 |
| --- | --- |
| `msvc2022` | 主目标，插件编译为 DLL |
| `msvc2022-static-plugins` | 插件链进主程序，调试器可直接单步进插件代码 |
| `msvc2019` | 回退工具链 |
| `ninja-debug` / `ninja-release` | 需要 ninja 在 PATH 上，迭代更快 |

产物都在 `build/<preset>/bin/`，插件在 `bin/plugins/`。

> 工程路径含中文时，MSVC 的 moc/rcc 在部分区域设置下可能出错。已在 CMake 里全局加了 `/utf-8`；
> 若仍报编码错，把工程移到纯英文路径即可。

## 运行

图形界面：

```powershell
build\msvc2022\bin\hwsim.exe
```

无头模式（CI 用）：

```powershell
hwsim.exe --headless `
          --workspace tests\fixtures\workspace.json `
          --script tests\fixtures\smoke-scenario.js `
          --log-level info
```

所有断言通过时退出码为 0，否则为 1。完整选项见 `hwsim --help`。

## 快速上手

1. 启动 `hwsim.exe`，菜单 **设备 → 添加设备**
2. 选协议（`modbus`）、角色（模拟设备 / 测试主站）、通讯方式（TCP 服务端）
3. 通讯和协议两个标签页由 schema 自动生成，填好端口和站号
4. 确定后设备自动启动，用任意 Modbus 主站软件连上来
5. 右侧 **参数** 页可直接改寄存器值，**场景** 页挂正弦波、注入 CRC 错误
6. 中间的报文视图实时显示收发，注入的故障会在"备注"列标出

## 项目结构

```
src/core/        基础设施：Result、TypeId、ByteBuffer、日志、事件总线、配置 Schema
src/transport/   通讯层：Transport（端点）/ Link（会话）二分
src/protocol/    协议框架：Command+Handler 派发、编解码接口、中间件、插件宿主
src/simulator/   设备仿真：参数树、状态机、信号源、故障注入
src/scripting/   QJSEngine 场景脚本
src/ui/          界面组件
src/app/         组装层与主窗口
plugins/         协议插件（modbus、私有协议模板）
tests/           单元测试、集成测试、样例档案
docs/            架构文档、插件开发指南
```

## 文档

- [架构设计](docs/architecture.md) — 分层、线程模型、UML、设计取舍
- [插件开发指南](docs/plugin-dev-guide.md) — 如何新增一个协议

## 扩展一个新协议

复制 `plugins/template/`，实现三样东西：

```cpp
// 1. 消息类型
struct ReadTemperature : protocol::MessageBase<ReadTemperature> {
    static constexpr protocol::OpCode opcode() { return 0x21; }
    static core::Result<ReadTemperature> decode(const protocol::Frame&);
    core::Result<QByteArray> encodeBody(const protocol::EncodeContext&) const;
    QString describe() const override;
};

// 2. 处理器（不继承任何东西）
class ReadTemperatureHandler {
public:
    core::Result<protocol::MessagePtr> handle(const ReadTemperature&, protocol::ExecutionContext&);
};

// 3. 一行注册
registry.bind<ReadTemperature>(std::make_shared<ReadTemperatureHandler>());
```

框架侧零改动，配置面板由 schema 自动生成。
