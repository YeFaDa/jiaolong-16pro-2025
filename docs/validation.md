# 实机验证记录

验证机器：

```text
MECHREVO / JIAOLONG Series
主板：JIAOLONG Series-X6xR55xK-B2
BIOS：N.1.16MRO14
EC firmware：1.20
内核：7.2.6
```

## ECRR 读取

只读模块通过 `INOU0000/ECRR` 成功读取：

```text
0x043e = 0x38
0x044c = 0x38
0x044f = 0x2f
0x0464/0x0465 = 0x07f9
0x046c/0x046d = 0x0813
0x0751 = 0x00
0x075b/0x075c = 0x3d/0x3d
0x0740 = 0x1a
0x078e = 0xec
```

RPM 按大端解释；`0x07f9` 为 2041 RPM，`0x0813` 为 2067 RPM。

## 动态监控

CPU 负载测试：

```text
idle:      temp=53/46 C, fan=1866/1886 RPM, pwm=78/78
load:      temp=73/48 C, fan=3084/3111 RPM, pwm=139/139
recovered: temp=59/47 C, fan=2739/2818 RPM, pwm=119/119
```

停止负载后温度和风扇自动下降，说明 EC 自动控制路径正常。

## QC71/WMI 读取对照

使用与 `qc71_laptop` 相同的 WMI GUID、方法 ID 和输入格式，固定设置 read flag：

```text
project 0x0740 = 0x1a
mode 0x0751 = 0x00
cpu_temp 0x043e = 0x3d
gpu_temp 0x044f = 0x35
fan_cpu 0x0464/0x0465 = 0x099a
fan_gpu 0x046c/0x046d = 0x09c5
duty 0x075b/0x075c = 0x52/0x52
fan_ctrl 0x078e = 0xec
qc71_pwm 0x1804/0x1809 = 0x51/0x51
```

WMI 读取与 ECRR 读取的项目 ID、模式、占空比和风扇数据一致。

## 单风扇写入验证

使用一次性测试模块：

```text
旧 CPU PWM = 0x4c
目标 CPU PWM = 0x32
```

结果：

```text
写入回读 = 0x32
10 秒后恢复 = 0x4c
模式前 = 0x00
模式后 = 0x00
```

该测试只写入 CPU 风扇 `0x1804`，没有修改 GPU PWM、风扇模式或曲线。

## 尚未验证

- GPU 风扇单独写入；
- 长时间低/高负载下的温度和风扇保护；
- suspend/hibernate 期间的恢复；
- 键盘背光写入；
- 性能模式 `0x0751` 写入；
- 自定义风扇曲线；
- BIOS/EC 固件更新后的兼容性。
