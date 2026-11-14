**语言：** [English](../../README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português (Brasil)](README.pt-BR.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [索引](README.md)

**Discord:** [discord.gg/urz7vMdRM](https://discord.gg/urz7vMdRM)

# Guarana

**面向集成显卡调优的 Nintendo Switch 2 模拟器。**

Guarana 是 [Lichii](https://github.com/zortos293/Lichii)（`nx2` 分支）的开源分支，面向 Windows x64。它专注于集成显卡的笔记本和掌机：先支持 Intel Arc，之后支持 AMD Radeon。重点是更均匀的帧节奏、帧生成、更快的启动，以及更合理的功耗分配。

> [!IMPORTANT]
> Guarana **不附带**密钥、固件、游戏或系统文件。你需要自己从你拥有的主机和游戏中提取。本项目与 Nintendo 及 Lichii 项目无关联。请参阅下文的法律说明。

## 这是什么

- 免费、遵循 GPL-3.0-or-later 许可的开源模拟器。
- 针对共享内存、功耗受限的集成 GPU 调优。
- 需要 Windows x64、支持 AVX2 的 CPU，以及较新的显卡驱动。
- 不包含任何密钥、固件、游戏或系统文件。

## 实测结果

在一台笔记本上测得：Core Ultra 9 285H，Arc 140T 集成显卡，Intel 驱动 32.0.101.9175，32 GB 内存。在《Ocarina of Time》的 Hyrule Field 场景中，全局内存路由修复（`EDEN_NX2_GM=11`）把游戏自身渲染的帧率从 **14.8 fps** 提升到 **22.8 fps**。测试时关闭了帧生成，并在同一位置做开关对比。

这只是一台设备、一款游戏、一个场景的数据。其他设备、驱动、游戏和场景尚未测量。欢迎[帮助测试](../../docs/BENCHMARKING.md)。

## 支持情况

目前只在 Intel Arc 140T 上测试过。其他显卡（包括 Arc 140V、B390、Radeon 780M、880M、890M 等）尚未测试，欢迎提交[设备报告](../../docs/DEVICES.md#send-a-device-report)。16 GB 共享内存的设备低于舒适下限，详见 [DEVICES.md](../../docs/DEVICES.md)。

## 快速开始

1. 从 [Releases](https://github.com/karimalsalah/guarana/releases) 下载最新的 zip 文件。这是唯一的官方下载地址。
2. 将整个 zip 解压到你有写入权限的文件夹，然后运行 **`guarana.exe`**。
3. 安装你自己的密钥和固件：依次点击 **Tools → Install Decryption Keys** 和 **Tools → Install Firmware**。
4. 添加游戏文件夹（双击空的游戏列表），然后启动游戏。每个区域第一次进入时会编译着色器并出现卡顿，之后会从缓存加载。
5. 可选：在 **Emulation → Configure… → Graphics** 中开启 **Enable Frame Generation**。

设置、存档和缓存位于 `%APPDATA%\guarana`。首次启动时，设置会自动从旧文件夹 `%APPDATA%\lichii` 迁移过来（旧文件夹保持原样）。所有开关见 [docs/switches.md](../../docs/switches.md)，遇到问题请看 [故障排除](../../docs/TROUBLESHOOTING.md)。

## 自备密钥、固件和游戏

- 请使用你从自己拥有的硬件中提取的密钥、固件和游戏。Guarana 不提供这些文件，也不接受在 issue、PR 或讨论区中分享它们。
- 我们不处理索取或分享受版权保护文件的请求，也不讨论获取此类文件的途径。
- Guarana 独立于 Nintendo 和 Lichii 项目，未获其认可或赞助。"Nintendo" 与 "Nintendo Switch" 是 Nintendo 的商标，仅用于说明本软件兼容的对象。完整说明见 [LEGAL.md](../../LEGAL.md)。

## 参与贡献

- **不写代码也能帮忙：** 运行[基准测试](../../docs/BENCHMARKING.md)并提交数据，或提交[设备报告](../../docs/DEVICES.md#send-a-device-report)。
- **写代码：** 从标有 `good first issue` 的 issue 开始，阅读 [CONTRIBUTING.md](../../CONTRIBUTING.md)，并参考[编译说明](../../docs/Build.md)。
- **AI 代理：** 请先阅读 [AGENTS.md](../../AGENTS.md)、[架构文档](../../docs/ARCHITECTURE.md)和 [AI 政策](../../docs/AI_POLICY.md)。每个 PR 都必须由人负责。

## 链接

- [英文 README（权威版本）](../../README.md) · [网站](https://karimalsalah.github.io/guarana/) · [常见问题](../../docs/FAQ.md)
- [路线图](../../ROADMAP.md) · [更新日志](../../CHANGELOG.md) · [许可证全文](../../LICENSE.txt)

## 许可证

Guarana 是自由软件，依据 **GNU General Public License v3.0 或更高版本** 授权。

本译文可能滞后于英文版。如两者有差异，以英文 README 为准。译文尚未经过母语人士审校。
