# 版本更新脚本任务

- 目标：支持默认 patch、minor/major、明确版本、current、--build/--no-build。
- 方案：Python 标准库脚本；同步 firmware/version.txt 与 server/yuanbao/__init__.py 的发布版本，初始 0.1.0。构建复用 tools/build-firmware.ps1。
- 当前进度：全部完成。脚本、README、问题、经验及修改记录已交付；6 项测试与 Ruff 检查通过。首次接入版本文件时的旧构建缓存问题已通过 CMake 依赖声明解决。
- 实际验证：python tools/version-bump.py 0.1.0 --build 返回 0；firmware/build/project_description.json 与 yuanbao_talk.bin 应用描述中的版本均为 0.1.0。没有烧录或部署。
- 验证边界：Windows ESP-IDF 6.1 实际构建已通过；Linux/macOS 构建分支未实机验证。无剩余任务。
- 边界：不修改协议或依赖版本，不自动提交、打标签、烧录或部署。
