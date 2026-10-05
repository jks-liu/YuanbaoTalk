# 缺少明确的发布版本来源

- 问题：固件未提供 version.txt 或 PROJECT_VER，服务器包未声明发布版本。工作目录没有 Git 元数据，不能依赖 Git 标签生成稳定的发布版本。
- 解决：以 0.1.0 初始化 firmware/version.txt 和服务器 __version__，由 tools/version-bump.py 同步维护。ESP-IDF 6.1 本地 project.cmake 已确认会读取 version.txt。
- 构建缓存：首次添加 version.txt 时，旧构建目录可能尚未跟踪该文件，直接 build 可能沿用旧版本。修改工程 CMakeLists.txt 并声明版本文件配置依赖，使现有构建目录重新配置。
- 注意：协议 version=1 是兼容性字段，不是发布版本，不能一起替换。依赖文件也不属于同步范围。
- 失败处理：更新前校验两个版本；普通写入异常尝试恢复原内容。多文件写入不承诺断电原子性，意外中断后由 current 检查一致性。构建失败保留版本并返回失败，以便针对同一版本重试。
