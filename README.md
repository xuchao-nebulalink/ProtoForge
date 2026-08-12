# 设备协议模拟平台

Qt 6 + C++20 的上位机设备协议模拟平台。双向对等：既能模拟下位机设备（从站）等上位机来连，
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

- **Qt** 6.5 或更高（开发环境 6.8.3），需勾选 Core / Network / SerialPort / Gui / Widgets / Qml / Test
- **MSVC** 2022，或 2019 16.11+（缺 `<format>`，构建时自动回退到 Qt 实现）
- **CMake** 3.22 或更高

> **工程路径必须是纯英文。** 含中文时，只要构建目录已存在，重新配置就会让 CMake 崩溃退出
> （`0xC0000409`，3.25 与 3.31 均复现）。首次配置反而是好的，所以这个坑往往等改了
> `CMakeLists.txt` 才炸出来。Visual Studio 会在构建脚本变动时自动重跑 CMake，因此中文路径下
> 用 VS 走不通。

## 构建

分两步：CMake 只负责生成解决方案，编译交给 Visual Studio。

```powershell
cmake --preset msvc2022
start build\msvc2022\HwSimPlatform.sln
```

解决方案打开后按 F5 直接跑。启动项已设为 `hwsim`，调试器的 `PATH` 里也注入了 Qt 的 `bin`，
不需要事先把 Qt 加进系统 PATH。也可以用 VS 的**打开文件夹**指向工程根，它会自己读
`CMakePresets.json`。

不开 IDE 就全走命令行：

```powershell
cmake --build --preset msvc2022-debug
ctest --preset msvc2022-debug
```

Qt 由 `cmake/QtSetup.cmake` 在 `C:/Qt`、`D:/Qt`、`E:/Qt` 下按当前编译器自动挑选。装在别处或要
固定版本，就设 `$env:QT6_DIR = "D:/QT/6.8.3/msvc2022_64"`。

| preset | 说明 |
| --- | --- |
| `msvc2022` | 主目标，插件编译为 DLL |
| `msvc2022-static-plugins` | 插件链进主程序，调试器可直接单步进插件代码 |
| `msvc2019` | 回退工具链 |
| `ninja-debug` / `ninja-release` | 需要 ninja 在 PATH 上，迭代更快 |

产物都在 `build/<preset>/bin/`，插件在 `bin/plugins/`。

## 运行

VS 里按 F5 即可。在 IDE 外运行要先让系统找得到 Qt：

```powershell
$env:PATH = "D:/QT/6.8.3/msvc2022_64/bin;$env:PATH"
build\msvc2022\bin\hwsim.exe
```

无头模式（CI 用）：

```powershell
hwsim.exe --headless `
          --workspace tests\fixtures\workspace.json `
          --script tests\fixtures\smoke-scenario.js
```

所有断言通过时退出码为 0，否则为 1。完整选项见 `hwsim --help`。

## 快速上手

1. 启动 `hwsim.exe`，菜单 **设备 → 添加设备**
2. 选协议（`modbus`）、角色（模拟设备 / 测试主站）、通讯方式（TCP 服务端）
3. 通讯和协议两个标签页由 schema 自动生成，填好端口和站号
4. 确定后设备自动启动，用任意 Modbus 主站软件连上来
5. 右侧 **参数** 页可直接改寄存器值，**场景** 页挂正弦波、注入 CRC 错误
6. 中间的报文视图实时显示收发，注入的故障会在“备注”列标出

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

## 文档

- [架构设计](docs/architecture.md) — 分层、线程模型、UML、设计取舍
- [插件开发指南](docs/plugin-dev-guide.md) — 如何新增一个协议
