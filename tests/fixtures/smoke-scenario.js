// Sample scenario script.
//
// Run headless in CI:
//   hwsim --headless --workspace workspace.json --script smoke-scenario.js
//
// The process exit code is 0 only when every check passes, so this file works
// directly as a regression gate.

hwsim.log("=== 设备协议模拟平台 冒烟场景 ===");

var devices = hwsim.devices();
hwsim.check(devices.length > 0, "工程中至少要有一台设备");

var dut = devices[0];
hwsim.log("被测设备: " + dut);

// --- 1. 启动并等待链路建立 -------------------------------------------------

hwsim.check(hwsim.start(dut), "设备应能启动");
hwsim.check(hwsim.waitForLink(dut, 1, 3000), "3 秒内应建立至少一条链路");

// --- 2. 参数读写 -----------------------------------------------------------

var setpoint = hwsim.read(dut, "hr.tempSetpoint");
hwsim.log("初始温度设定: " + setpoint);

hwsim.write(dut, "hr.tempSetpoint", 720);
hwsim.checkEqual(hwsim.read(dut, "hr.tempSetpoint"), 720, "写入后应读回新值");

// --- 3. 状态机 -------------------------------------------------------------

hwsim.log("当前状态: " + hwsim.state(dut));
hwsim.postEvent(dut, "start");
hwsim.check(hwsim.waitForState(dut, "Heating", 2000), "触发 start 后应进入 Heating");

// 进入 Heating 时的 onEnter 应把燃烧器打开
hwsim.checkEqual(hwsim.read(dut, "coil.burner"), true, "Heating 状态下燃烧器应开启");

// --- 4. 实时数据在变化 -----------------------------------------------------

var first = hwsim.read(dut, "ir.waterTemp");
hwsim.sleep(600);
var second = hwsim.read(dut, "ir.waterTemp");
hwsim.check(first !== second, "正弦 + 噪声信号源应让水温持续变化");

// --- 5. 故障注入 -----------------------------------------------------------

var lossRule = hwsim.injectFault(dut, "packet-loss", {
    direction: "outbound",
    trigger: "always"
});
hwsim.check(lossRule.length > 0, "应能注入丢包规则");

hwsim.sleep(300);
hwsim.check(hwsim.clearFault(dut, lossRule), "应能移除丢包规则");

// 校验错误规则改用手动触发，只影响下一帧
var crcRule = hwsim.injectFault(dut, "checksum-error", {
    direction: "outbound",
    trigger: "manual",
    checksumBytes: 2
});
hwsim.check(hwsim.armFault(dut, crcRule), "应能手动装填校验错误");
hwsim.sleep(200);
hwsim.clearFault(dut, crcRule);

// --- 6. 设备失联与恢复 -----------------------------------------------------

hwsim.postEvent(dut, "lockout");
hwsim.check(hwsim.waitForState(dut, "Lockout", 2000), "应能进入不应答状态");

hwsim.postEvent(dut, "reset");
hwsim.check(hwsim.waitForState(dut, "Standby", 2000), "复位后应回到 Standby");

// --- 收尾 ------------------------------------------------------------------

hwsim.stop(dut);
hwsim.log("=== 场景执行完毕 ===");
