# JIAOLONG 16 Pro 2025 Linux 支持

这是一个面向 **MECHREVO 蛟龙 16 Pro 2025** 的实验性 Linux 内核模块与 NixOS 打包项目,由opencode完成。

控制模块不是标准 hwmon PWM 接口，目标是让 fancontrol 能识别需要改成：
pwm1 / pwm2
pwm1_enable / pwm2_enable
写入仍保留精确 DMI、最小 PWM、租约和自动恢复

它不是通用 Uniwill 驱动，也不会通过 `force=1` 加载到未知机器。项目目前包含：

- 基于 `INOU0000/ECRR` 的只读 EC 监控模块；
- 参考 `qc71_laptop` WMI 事务格式的蛟龙风扇控制模块；
- NixOS 模块和按当前内核构建的外部模块 derivation；
- QC71/WMI 只读对照探针和实机验证记录。

> **警告：EC 写入可能导致风扇失控、EC 固件异常、系统 BSOD（包括 BSOD 0xA5）。本项目默认关闭所有写入。只有在确认自己的 BIOS/EC 固件和测试条件后，才可以显式开启。**

## 实机支持范围

目前只对以下机器启用严格匹配：

| 项目 | 值 |
|---|---|
| 厂商 | `MECHREVO` |
| 产品 | `JIAOLONG Series` |
| 主板 | `JIAOLONG Series-X6xR55xK-B2` |
| BIOS | `N.1.16MRO14` |
| EC 固件 | `1.20` |
| EC project ID | `0x1a` |
| 内核 | Linux 7.2.6（已测试） |
| 架构 | x86_64 |

DMI、BIOS、EC 固件或 project ID 不匹配时，模块会拒绝 probe。不会因为存在相同 WMI GUID 就自动加载。

## 已验证功能

### 只读监控

注意：CPU 温度已经可以由 `k10temp` 读取，GPU 温度也已经可以由 `amdgpu` 读取；蛟龙固件本身还会在 EC/BIOS 内部自动运行风扇曲线。因此，这个模块的独有价值不是“首次提供 CPU/GPU 温度”，而是补上 Linux 当前缺失的：

- 两个实体风扇的 RPM；
- EC 实际占空比；
- 风扇模式和 EC 能力状态；
- 后续控制所需的安全读写基线。

`drivers/jialong-ec-monitor` 使用蛟龙固件提供的 `INOU0000/ECRR` 方法，暴露标准 hwmon 只读接口：

- CPU 温度：`0x043e`
- GPU 温度：`0x044f`
- CPU 风扇 RPM：`0x0464/0x0465`，大端
- GPU 风扇 RPM：`0x046c/0x046d`，大端
- 两个风扇占空比读数：`0x075b`、`0x075c`
- 模式原始值：`0x0751`

实机验证过动态行为：

- CPU 温度从 53°C 负载上升到 73°C；
- CPU 风扇从约 1866 RPM 上升到约 3084 RPM；
- GPU 风扇同步上升；
- 停止负载后温度和风扇自动回落。

### QC71/WMI 风扇控制

`drivers/jialong-fan-control` 借鉴 `qc71_laptop` 的：

- WMI GUID `ABBC0F6F-8EA1-11D1-00A0-C90629100000`；
- 方法 ID `4`；
- 8 字节 EC 事务格式；
- `0x1804`、`0x1809` 风扇 PWM 寄存器；
- `qc71` 风格的 PWM 换算和模式恢复思路。

本机只读对照结果：

```text
project 0x0740 = 0x1a
0x1804 = 0x51
0x1809 = 0x51
0x075b = 0x52
0x075c = 0x52
0x078e = 0xec
```

一次受控写入测试已经完成：

```text
旧 PWM 0x4c
写入 0x32
立即回读 0x32
10 秒后恢复 0x4c
模式保持 0x00
```

控制模块的安全限制：

- 默认 `control_enable=false`；
- 模块加载时绝不写 EC；
- 写入接口是 root-only、write-only 的 `fan_control` 属性；
- 原始 PWM 限制为 `50..200`，不暴露风扇关闭；
- 第一次写入前保存两个 PWM 和模式；
- 每次控制有 30 秒租约；
- 超时、卸载或写入校验失败时恢复保存值；
- 只允许固定的两个 PWM 地址，不接受任意 EC 地址。

## 目录结构

```text
.
├── drivers/
│   ├── jialong-ec-monitor/       # 只读 ECRR + hwmon
│   └── jialong-fan-control/      # 可选 QC71/WMI 风扇控制
├── nix/
│   └── module.nix                # NixOS 模块
├── research/
│   ├── qc71-read-probe.c         # 只读 WMI 对照探针
│   └── Makefile
├── docs/
│   └── validation.md             # 实机验证记录
├── LICENSE
└── README.md
```

## Flake 构建

项目提供 Flake 输出：

```bash
nix build .#jialong-ec-monitor
nix build .#jialong-fan-control
```

两个输出都绑定 `pkgs.linuxPackages_latest.kernel`；在 NixOS 中应使用与目标系统一致的 `boot.kernelPackages.kernel`。

## NixOS 使用

在本机 flake 的模块列表中加入：

```nix
imports = [ /path/to/jiaolong-16pro-2025/nix/module.nix ];
```

只开启监控：

```nix
boot.jialongEcMonitor = true;
```

加载控制模块但保持写入关闭：

```nix
boot.jialongFanControl = true;
boot.jialongFanWrite = false;
```

`boot.jialongFanControl = true` 只加载控制模块；它仍处于只读安全模式，不会创建可写接口，也不会写 EC。

显式开启租约写入接口需要：

```nix
boot.jialongFanControl = true;
boot.jialongFanWrite = true;
```

这会向内核命令行加入：

```text
jialong_fan_control.control_enable=1
```

**这属于 EC 写入功能。除非已经完成自己的实机验证，否则不要启用。**

## 安全和限制

- EC 写入不是普通硬件寄存器操作，固件可能拒绝、忽略或误解写入；
- 不要在电池、电容或固件状态不稳定时测试；
- 风扇测试应保持一个最小安全占空比，不要测试 `0`；
- 每次只改一个风扇，写后立即回读；
- 任何异常都应先恢复自动模式，必要时重启；
- 当前没有实现键盘背光写入和完整性能模式控制；
- 当前没有保证支持其他 MECHREVO/Uniwill/TUXEDO 机型；
- project ID `0x1a` 只能作为本机识别条件，不能单独作为通用白名单。

## 开源价值与定位

这个项目的价值不在于重复提供 CPU/GPU 温度：`k10temp` 和 `amdgpu` 已经可以提供这些温度，固件本身也会自动运行风扇曲线。

它的独有价值是：

- 暴露 Linux 当前缺失的两个实体风扇 RPM、EC 占空比和风扇状态；
- 提供经过 DMI/固件/project ID 限制的蛟龙风扇写入路径；
- 记录 QC71/WMI 与蛟龙 ACPI/EC 寄存器之间的可复现实验结果；
- 为 NixOS 用户提供可构建、可审计的外部模块；
- 为后续键盘背光、性能模式和风扇曲线控制提供基础。

因此它应被定位为**实验性硬件支持项目**，而不是通用 Uniwill 驱动。温度读取本身不是独占功能；真正的价值在风扇可见性、控制和自动化。

## 致谢和许可证

本项目代码以 **GPL-2.0** 发布，详见 [`LICENSE`](LICENSE)。

参考和借鉴：

- [`pobrn/qc71_laptop`](https://github.com/pobrn/qc71_laptop)：WMI EC 事务和风扇控制结构，GPL-2.0；
- [`Wer-Wolf/uniwill-laptop`](https://github.com/Wer-Wolf/uniwill-laptop)：Uniwill ACPI 传感器和寄存器语义；
- [`roxyyn0304/jialong-control-protocol`](https://github.com/roxyyn0304/jialong-control-protocol)：蛟龙控制协议和实机寄存器研究，MIT；
- Linux kernel ACPI/WMI/hwmon 子系统。

第三方项目仍受各自许可证约束。
