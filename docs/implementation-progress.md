# 实现任务交接

更新时间：2026-10-03；用户已授权按照 v0.2 规格实现固件与服务器。

## 环境与边界

- ESP-IDF 6.1：加载 `C:/Espressif/tools/Microsoft.v6.1.PowerShell_profile.ps1`。
- 目标 ESP32-S3 / 16 MB PSRAM。用户确认硬件尚未选型、做成可配置项；实现标准 I2S 板级适配，默认不驱动未知引脚。
- 不烧录未知硬件，不宣称 AEC 音质或最大混音路数已实测。

## 已完成的软件实施计划

1. 建立 ESP-IDF 工程与固定依赖，验证环境。
2. 实现线协议、去重出口状态与服务器；用跨连接集成测试验证防回环/绑定/容量。
3. 实现 ESP-NOW、自动 WebSocket 网关、设置持久化、双向音频和前端。
4. 编译 S3 固件、运行主机测试，补齐使用说明与剩余硬件验收项。

## 进度

- 已建立固定依赖的 ESP-IDF 工程及 Python asyncio 服务器。
- 已实现共同线格式、频道隔离、去重窗口与独立出口、TTL、防服务器回灌、ESP-NOW 中继、固定信道 Wi-Fi、自动双 WebSocket 网关和重连。
- 已实现服务器绑定/媒体重绑、流准入、控制幂等、限流、过期队列和慢消费者隔离；15 项服务器测试通过。
- 已实现双向 I2S、PTT、每麦 AEC、延迟求和波束形成、NS、Opus、逐源抖动缓冲/漂移补偿/丢包掩蔽、混音和性能降档；串口设置持久化。
- 默认、完整音频运行入口及核心自测固件均成功构建；QEMU 最终复跑通过协议、路由、Opus、四路混音/降档、丢包衰减及堆检查，自测最低剩余栈 29580 字节。这不是四路实时负载验收。
- AEC 创建成功，执行时 Windows QEMU 宿主崩溃，详见 issues/009；AEC/NS 运行及声学效果未通过验收。保留独立 `-Afe` 自测配置。
- 服务器 15 项回归、Python Ruff 与 C clang-format 检查通过；本轮软件实现、构建和文档收尾完成，未烧录或部署。下一阶段为下面的实机验收。

## 验证证据

| 项目 | 结果与产物 |
|---|---|
| 默认固件 | `firmware/build/yuanbao_talk.bin`，805472 字节；`firmware/build-check.log` |
| 完整音频运行入口 | `firmware/build-audio/yuanbao_talk.bin`，1094016 字节；`firmware/audio-build.log`。确认 SELF_TEST=false、AUDIO_ENABLE/BEAMFORM=true，ELF 含 aec_process、ns_process、audio_task、yt_gateway_start |
| 核心自测 | `firmware/build-selftest/yuanbao_talk.bin`，368960 字节；`firmware/build-selftest/selftest.log` 含 PASS，AEC/NS 明确 NOT TESTED |
| 服务器 | `tools/test-server.ps1`：15 项通过，含真实 WebSocket 多网关、频道隔离、绑定重放、媒体重绑、并发准入、乱序收尾及慢消费者 |
| 格式与静态检查 | Ruff check/format 与 clang-format dry-run 通过；最终三份构建日志无 compiler warning/error |

生成目录和日志被 `.gitignore` 排除；按 README 和 tools 脚本可重建。`-Afe` 是保留给兼容执行环境的完整算法冒烟配置，其处理路径尚未通过运行验收。

## 后续实机验收

详细步骤见 [hardware-validation.md](hardware-validation.md)。

- 确定双麦/功放型号、接线及 Flash 容量，校准参考延迟、麦间距与波束方向。
- 三台以上设备联调直连/中继/多个在线网关、断网恢复、频道切换、拥塞与 TTL。
- 在真实采集/播放、双麦 AEC/NS、编码及网络并发下测量混音上限、栈/堆、欠载和端到端延迟；默认未验证板型上限为 1，不承诺四路实时性能。
- 采集溢出使时间轴不可信，当前明确暂停发送直到重启，接收继续。
