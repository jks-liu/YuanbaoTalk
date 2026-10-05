# ESP-IDF 6.1 的 JSON 组件名称迁移

首次固件配置失败：`Failed to resolve component 'json'`。旧版 IDF 中的 JSON 组件依赖不能直接沿用到本环境。

处理：使用托管 `espressif/cjson`，在 main 组件声明对应依赖；官方音频依赖解析时已固定其版本。随后配置和默认固件编译通过。不要为解决此问题复制整个旧版 IDF JSON 源码进项目。

另外，本机 IDF 提示 PowerShell Unicode 环境未配置，构建脚本设置 `PYTHONUTF8=1`，无需修改用户全局系统语言设置。
