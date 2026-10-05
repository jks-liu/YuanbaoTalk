# Webmaku

## 插件介绍

这是一个名为YuanbaoTalk的软件，其使用esp32 now实现的对讲机，有中继功能，可以直连，也可以连接服务器。

其包含两个部分
- 固件：功能参考`docs/YuanbaoTalk-firmware-spec.md`
- 服务器：功能参考`docs/YuanbaoTalk-server-spec.md`

## 开发notes

- docs中新建一个目录（issues），记录你工作过程中遇到的问题和解决方法，每个问题使用详细简练的中文保存到一个md文档中。
- docs中新建一个目录（lessons），记录你认为值得记录的经验、教训、总结等，每个记录使用详细简练的中文保存到一个md文档中。
- docs中新建一个md文档，记录你工作过程中的关键修改（包括修改原因），使用详细简练的中文并经常更新
- 保存一份任务plan和进度文档，目的是即使上下文丢失也能获取任务内容和当前进度并继续工作。此文档的目的是交接，不是流水账，不要包含过时的内容。此文档应定期更新。
- 通用/可复用的辅助脚本放在tools文件夹里，包含必要的注释、文档和使用方法。

- ESP-IDF 6.1的环境是C:/Espressif/tools/Microsoft.v6.1.PowerShell_profile.ps1
- 官方例程在`C:/esp/v6.1/esp-idf/examples`，供你参考
