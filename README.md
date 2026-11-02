# FGB VirtualDesktopModule

[![build](https://github.com/Aqamoe/FGB-VirtualDesktopModule/actions/workflows/ci.yml/badge.svg)](https://github.com/Aqamoe/FGB-VirtualDesktopModule/actions/workflows/ci.yml)

**状态：0.1.0，未验证（untested）**。结构偏移、映射与 ABI 生命周期有单元测试和
「自建同名共享内存喂合成帧」的端到端冒烟；真机（VD Streamer ≥1.30 + 支持眼动的头显）
验收待补，见 [docs/verification.md](docs/verification.md)。

[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge) 的 Virtual Desktop 原生模块。
直接读取 **Virtual Desktop 串流端**转发到本机的命名共享内存
`VirtualDesktop.BodyState`（360 字节的 FaceState）与事件 `VirtualDesktop.BodyStateEvent`，
输出 Unified Expressions 表情、逐眼睁眼度与注视方向。不需要 VRCFT，不注入任何进程，
也不需要管理员权限。

Virtual Desktop native input module for FaceGaze Bridge. Reads the named shared
memory (`VirtualDesktop.BodyState`) and event that the VD Streamer already
publishes, and outputs Unified Expressions shapes, per-eye openness and gaze
direction. No VRCFT, no process injection, no admin rights.

## 功能 / Features

- **表情**：70 个 OpenXR 风格权重 → 69 个 Unified Expressions 形状，
  映射口径逐行核对自上游 VRCFT 模块（含睁眼度、上唇减鼻皱、吸唇幂次三个公式）。
- **眼睛**：逐眼睁眼度 + 注视方向（双眼 Pose 四元数 → 头局部单位向量，-Z 前）；
  只在该眼 `*EyeIsValid` 时写该眼，绝不连坐。
- **防御式读取**：该结构**没有版本号**，模块做映射长度校验、越界拒绝、双快照
  半更新识别、全零识别、合理性漂移哨兵与 NaN 清洗（详见
  [docs/protocol.md](docs/protocol.md)）。
- **断连可恢复**：读不到映射/没数据时，提示直接指向「是否开启了面部追踪转发」，
  且按间隔重试不刷屏；VD 启动后自动接入，断流 10 秒报警、恢复自动回到活跃。

## 不提供 / Not provided

| 数据 | 原因 |
|---|---|
| 头姿 | FaceState 里没有头姿字段（Pose.Position 是眼位置，坐标系未确认） |
| 瞳孔 | 上游写死 5mm 属伪造值，本模块不输出，也不置 PUPIL 位 |
| 2D 注视点 | 协议没有；注视是设备级方向，未经标定不置 POINT_VIEWPORT |
| `ExpressionConfidences[2]` / 逐眼置信度的确切含义 | 上游不消费、语义未确认（见 protocol.md） |

## 环境要求

| 项 | 要求 |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| 宿主 | FaceGaze Bridge（原生模块 ABI v2） |
| 串流 | Virtual Desktop Streamer **≥1.30**，设置里开启面部追踪转发 |
| 头显 | VD 设置里启用面捕（需头显本身支持眼动，如 Quest Pro） |
| 其它 | 关闭 VRCFaceTracking：它的 Virtual Desktop 模块读同一命名对象 |

## 安装

1. 从 Releases 下载 `virtual-desktop-0.1.0.zip`。
2. FaceGaze Bridge →「模块库 → 原生模块 → 安装原生模块 (.zip)」，或「模块」页「从 .zip 安装」。
3. 「模块」页把 **Virtual Desktop** 设为启动，角色通常给 `expression` 与 `eyes`（要注视通道再给 `gaze`）。
4. 重启引擎；戴上头显开始串流、头显端面捕激活后，模块卡片应显示活跃。

## 配置

无需配置文件：映射与事件的名字来自上游协议，不是可调项。VD 没在运行时模块会
按间隔静默重试，串流一开始即自动接入。

## 从源码构建

需要 Visual Studio 2022（x64）、CMake ≥ 3.24 与 Node 20（脚本用）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

SDK 与 Kit 由 `sdk.lock.json` / `kit.lock.json` 在配置阶段拉取并校验 SHA256，仓库里不含 SDK 头文件副本。
本地想指向已解压的 Kit：

```powershell
cmake -S . -B build -A x64 -DFGB_KIT_DIR="D:/code/FGB-Module/FGB-Module-Kit"
```

打包与测试：

```powershell
cmake --build build --config Release --target package-modules   # build/Release/virtual-desktop-<版本>.zip
ctest --test-dir build -C Release --output-on-failure           # 布局/映射/防御单测
cmake --build build --config Release --target smoke-test        # 真实 ABI + 合成帧冒烟（勿与运行中的 VD Streamer 同机撞名）
```

上游 C# 参考源码（仅协议核对用，不入库）放在 `.tmp-upstream/vd`。

## 目录结构

```
src/main.cpp                ABI 生命周期、共享内存/事件接入、状态上报
src/vd_payload.*            FaceState 布局镜像、校验清洗、映射与断流监控
module/                     fgb-native.json 与图标
tests/                      布局与映射单测、真实 ABI 冒烟
docs/protocol.md            偏移表（已确认/未确认）与映射依据
docs/verification.md        真机验收清单
cmake/                      依赖锁解析（SDK / Kit 都是拉取，不复制）
```

## 排错

| 现象 | 处理 |
|---|---|
| 卡片显示「还没有给出可用面捕帧」 | 依次确认：VD Streamer ≥1.30 在跑、串流端开了「Forward tracking data to PC」、头显侧启用了面捕、正在串流 VR 应用 |
| 日志出现「内容全零」 | 映射在但没数据：多半是头显端面捕没启用，或转发开关没生效 |
| 日志出现「数值校验失败/结构布局」 | VD 更新改了字段的可能性；先对照 docs/protocol.md 的漂移风险，附版本号提 issue |
| 表情动、注视不动 | 头显没有眼动硬件或该眼 `IsValid` 没置位 |
| VRCFT 与本模块同时开 | 关掉其中一个：两边读同一 VirtualDesktop.BodyState 会互相干扰日志 |

## 致谢 / Attribution

- 字段布局与映射依据一手核对自
  [VirtualDesktop.VRCFaceTracking](https://github.com/guygodin/VirtualDesktop.VRCFaceTracking)
  （Virtual Desktop, Inc.，MIT）；本仓库是该线格式的独立 C++ 实现，未包含其源码。
- ABI 头与宿主：[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge)；共享工具链：[FGB-Module-Kit](https://github.com/Aqamoe/FGB-Module-Kit)。

## 许可

Apache License 2.0，见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。
