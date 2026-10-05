# YuanbaoTalk

ESP32-S3 ESP-NOW 多人对讲：无线中继、自动公网网关、接收端混音、PTT 期间收听。服务器用 Python asyncio/WebSocket 转发原始语音流。协议见 [共同规格](docs/YuanbaoTalk-protocol-spec.md)。

## 目录

- `firmware/`：ESP-IDF 6.1 工程；协议、去重路由、网关、NVS 设置和双麦音频。
- `server/`：单进程频道所有者、控制/媒体 WebSocket 和自动测试。
- `tools/`：构建、测试和模拟器辅助脚本。
- `docs/implementation-progress.md`：当前实施状态与硬件验证边界。

## 服务器

需要 Python 3.11 或更高版本。Windows PowerShell：

```powershell
python -m venv .venv
./.venv/Scripts/python.exe -m pip install -r server/requirements.txt
$env:PYTHONPATH = "$PWD/server"
./.venv/Scripts/python.exe -m yuanbao.server --host 0.0.0.0 --port 8765
```

Linux/macOS 使用 `.venv/bin/python` 和 `PYTHONPATH=server`。默认频道容量 8 源，可用 `--streams` 设置；`--ttl 1..16` 控制服务器下行的目标无线段预算，`--connections` 设置控制会话上限。可选 `--token` 与设备配置相同，仅作接入控制。第一版不要求加密，默认 ws 不提供链路保密。

接口为 `/v1/control` 和 `/v1/media`；固件填写 `ws://服务器地址:8765`，不加结尾斜杠。设备主动连接，无需手机或指定专用网关。后台接入者应遵守 HELLO、时间同步、JOIN、BIND、流注册的次序。

## 固件构建与配置

```powershell
./tools/build-firmware.ps1 menuconfig
./tools/build-firmware.ps1 build
```

脚本使用用户指定的 `C:/Espressif/tools/Microsoft.v6.1.PowerShell_profile.ps1`，设置 Python UTF-8。组件版本见 `firmware/main/idf_component.yml`，实际依赖锁定在 `firmware/dependencies.lock`。

在 `YuanbaoTalk` 菜单设置业务频道、离线/AP 无线信道、SSID/密码和服务器地址。AP 必须固定为同一无线信道；其他无线信道的同频道设备可通过服务器互通。留空 SSID 或服务器地址即可纯 ESP-NOW 工作。广播转发可关闭，TTL 默认建议 3。

目标为 S3、Octal PSRAM，应用需要 PSRAM 保存有界转发窗口。当前分区模板按至少 8 MB Flash 配置；PSRAM 的 16 MB 与 Flash 容量是两个参数，应按实际模组配置。未发现 PSRAM 时明确停止路由启动。

### 音频板级配置

板子尚未选型，默认不启用音频，GPIO 均为 -1。选定引脚后再勾选 `Enable configured audio hardware`：

- 共时钟标准 Philips I2S，16 kHz、左右双槽、每槽 32 位；两麦输入和扬声器输出共用 BCLK/WS。
- DIN 接左右声道双麦数据；DOUT 输出左右相同的单声道播放；可配置 MCLK、PCM 位移和低电平 PTT GPIO。
- 当前适配器针对标准数字 I2S 麦克风/功放。若器件需要 I2C 寄存器初始化、PDM 或 TDM，应按选定器件增加板级适配，不能只填 GPIO 就认为支持所有 codec。
- AEC 使用每麦独立的 full-duplex 模式；参考由实际播放 DMA 回调取得。`AEC_REFERENCE_DELAY` 用于板级延迟校准。
- 双麦 beamforming 在确定间距/方向/通道相位后启用，配置带符号的 Q8 采样延迟及右麦增益。未校准默认使用左麦处理结果。
- NS 默认启用，强度 0/1/2 可配置；前端持续在本地运行，但仅按住 PTT 才上传采集音频。

### 设备设置

串口控制台提供以下命令，设置保存到 NVS：

```text
status
mix auto
mix 2
volume 70
ttl 3
channel 123
channel 5f180b62-879b-41fa-8ee5-ea641f317ba0
```

`channel` 保存后重启以清除旧连接与播放状态。`mix` 手动值必须在板级验证上限内；未验证板型默认上限为 1。自动模式在该上限内按处理耗时保护性降档，持续稳定后恢复。扩大 `YT_VERIFIED_MIX_MAX` 前必须完成完整音频与网络负载验收。PTT 不关闭扬声器，降低本机混音路数也不会裁剪网关转发的源。

## 发布版本

使用 Python 3.11+，不需要 npm 或额外 Python 依赖：

```powershell
python tools/version-bump.py                   # 默认递增 patch
python tools/version-bump.py patch --build     # 更新并构建固件
python tools/version-bump.py minor
python tools/version-bump.py major
python tools/version-bump.py 1.0.0              # 设置明确版本（允许降级）
python tools/version-bump.py patch --no-build  # 默认不构建
python tools/version-bump.py current           # 只读查询并检查版本一致性
```

初始发布版本为 `0.1.0`，同步保存在 `firmware/version.txt`（ESP-IDF 自动读取）和 `server/yuanbao/__init__.py` 的 `__version__` 中。只支持无前导零的 `X.Y.Z`，长度最多 31 个 ASCII 字符，以适配固件版本字段；不支持预发布或构建后缀。协议中的版本 `1` 和第三方依赖版本不随发布版本改变。

版本不一致或文件缺失时会报错且不更新文件；需先手动核对恢复一致。`current` 不允许搭配 `--build`。构建失败返回非零退出码，已更新的版本保留；修复后直接运行固件构建脚本，或指定同一个明确版本搭配 `--build`，避免再次递增。

Windows 的 `--build` 调用现有 `tools/build-firmware.ps1`，Linux/macOS 则要求先激活 ESP-IDF 环境，再调用 `idf.py build`。服务器直接运行 Python 源码，无需构建。脚本不自动提交、打标签、烧录或部署。从其他目录调用时，使用脚本绝对路径即可。

## 验证

```powershell
python -m unittest discover -s tools -p test_version_bump.py -v
./tools/test-server.ps1
./tools/build-selftest.ps1
./.venv/Scripts/python.exe tools/run-selftest.py
./tools/build-firmware.ps1 -AudioCheck
```

自测配置使用示例 GPIO，启动后只运行协议、路由、codec、丢包掩蔽及四路混音逻辑自测，不驱动真实音频引脚。`-AudioCheck` 另外编译并链接完整音频运行入口。两者均不能当作实际板卡接线配置。

当前 Windows QEMU 在 AEC 处理时宿主崩溃，核心自测明确跳过 AEC/NS；`./tools/build-selftest.ps1 -Afe` 可构建完整算法冒烟自测，产物在 `firmware/build-selftest-afe/`，需支持的执行环境继续验证。详见 [问题记录](docs/issues/009-audio-stack-and-qemu.md)。模拟器不验证无线覆盖、麦克风同步、声学回声效果或实际并发性能。

音频任务栈为 48 KB，日志含最低剩余栈量；丢包时重复上一 PCM 块并在 120 ms 内渐弱，不调用不支持的 NULL 输入解码。若出现采集队列溢出，时间轴不再可靠，会明确暂停发送直到重启，接收仍继续。

依赖格式检查工具可通过 `pip install -r tools/requirements-dev.txt` 安装。固件编译产物在 `firmware/build/`，自测产物在 `firmware/build-selftest/`；没有自动烧录或部署。
