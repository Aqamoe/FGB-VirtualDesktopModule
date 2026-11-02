# Virtual Desktop 面捕共享内存协议要点

一手来源：[guygodin/VirtualDesktop.VRCFaceTracking](https://github.com/guygodin/VirtualDesktop.VRCFaceTracking)
（C#/MIT，`TrackingModule.cs`、`FaceState.cs`、`Expressions.cs`、`Pose.cs`、
`Quaternion.cs`，main 分支 2026-11 拉取，插件程序集版本 1.1 / module.json 版本 1.5）。
本仓库是该**线格式的独立 C++ 实现**，未复制上游代码；下列偏移/大小全部按上游
`[StructLayout(LayoutKind.Sequential)]` 声明顺序逐字段推算并与实际数值比对
（`FaceState.cs` 未使用显式 `FieldOffset`，顺序布局 + 自然对齐即下表）。

## 命名对象（已确认）

| 对象 | 名字（上游字符串字面量） | 类型 |
|---|---|---|
| 状态映射 | `VirtualDesktop.BodyState` | 命名页文件映射，VD Streamer 写 |
| 帧事件 | `VirtualDesktop.BodyStateEvent` | 命名事件，写完一帧 SetEvent |

- **无 `Global\`/`Local\` 前缀是上游原样**（已确认：`TrackingModule.cs` 的
  `BodyStateMapName`/`BodyStateEventName` 字面量）。.NET 把名字原样传给
  `OpenFileMapping`/`OpenEvent`（已确认：dotnet/runtime `MemoryMappedFile.Windows.cs`
  的 `OpenCore` 不加工 mapName）；Windows 对无前缀名按**当前会话的 BaseNamedObjects
  （Local 命名空间）**解析（已确认：MSDN 命名对象文档「未指定前缀时默认 Local」）。
  VD Streamer 与 VRCFT/FGB 都运行在用户桌面会话，所以我们也用同一裸名。
  **若哪天 Streamer 改以服务（Session 0）运行，裸名将互相看不见**——这是整条链路的
  隐性前提之一。
- 上游以 `MemoryMappedFileRights.ReadWrite` 打开；我们只读（`FILE_MAP_READ`）即可。
- 名字虽叫 BodyState（同一映射还承载身体追踪，整图尺寸上游面捕模块不关心），
  但 **FaceState 从映射偏移 0 开始**——已确认：上游 `CreateViewAccessor(0,
  Marshal.SizeOf<FaceState>())` 后直接把视图指针 cast 成 `FaceState*`。
  映射总大小（≥4096 页粒度）与其余字段布局：**未确认**，本模块只校验
  「可读长度 ≥ 360 字节」。

## FaceState 布局（已确认，共 360 字节）

C# 声明：`[StructLayout(LayoutKind.Sequential)]`，**未写 Pack**，即默认打包
（blittable 结构按各字段自然对齐）。所有字段是 1 字节（byte/bool I1）或 4 字节
（float），因此 Pack 取 1/4/8 结果一致：自然对齐无重排，总大小 360、对齐 4。
`bool` 均带 `[MarshalAs(UnmanagedType.I1)]` → 1 字节真值（已确认）。

字节序与浮点：x64 Windows 原生小端；float 为 IEEE-754 binary32——上游是进程内
指针 reinterpret，没有显式序列化规定，这是**平台事实**（标注：按平台默认，未单独声明）。

| 偏移 | 大小 | 字段（C#） | 说明 | 状态 |
|---|---|---|---|---|
| 0 | 1 | `FaceFlags` | bit0=表情权重可用；bit1=舌头槽位切换；其余位上游未用 | 用法已确认，其余位语义未确认 |
| 1 | 1 | `IsEyeFollowingBlendshapesValid` | 眼睑/眉组与睁眼度的闸门 | 已确认 |
| 2 | 2 | （对齐填充） | | 已确认（推导） |
| 4 | 280 | `ExpressionWeights[70]` | 下标表见下 | 已确认 |
| 284 | 8 | `ExpressionConfidences[2]` | 两个置信度浮点；上游面捕模块**不消费** | 存在已确认，语义未确认 |
| 292 | 1 | `LeftEyeIsValid` | | 已确认 |
| 293 | 1 | `RightEyeIsValid` | | 已确认 |
| 294 | 2 | （对齐填充） | | 已确认（推导） |
| 296 | 28 | `LeftEyePose` | `Quaternion Orientation{X,Y,Z,W}`(296..311) + `Vector3 Position`(312..323) | 布局已确认；坐标系按 OpenXR 约定推断（-Z 前 +Y 上），**未真机确认** |
| 324 | 28 | `RightEyePose` | 同上（Orientation 324..339，Position 340..351） | 同上 |
| 352 | 4 | `LeftEyeConfidence` | 上游不消费；本模块用作该眼注视置信度（按名推断） | 存在已确认，语义未确认 |
| 356 | 4 | `RightEyeConfidence` | 同上 | 同上 |
| — | — | **sizeof = 360** | `Marshal.SizeOf<FaceState>()`；本仓库以 `static_assert` + 运行期 `offsetof` 双重锁死 | 已确认（推导 + 断言） |

## 70 个权重下标（已确认，`Expressions.cs`）

0 BrowLowererL · 1 BrowLowererR · 2 CheekPuffL · 3 CheekPuffR · 4 CheekRaiserL ·
5 CheekRaiserR · 6 CheekSuckL · 7 CheekSuckR · 8 ChinRaiserB · 9 ChinRaiserT ·
10 DimplerL · 11 DimplerR · 12 EyesClosedL · 13 EyesClosedR · 14 EyesLookDownL ·
15 EyesLookDownR · 16 EyesLookLeftL · 17 EyesLookLeftR · 18 EyesLookRightL ·
19 EyesLookRightR · 20 EyesLookUpL · 21 EyesLookUpR · 22 InnerBrowRaiserL ·
23 InnerBrowRaiserR · 24 JawDrop · 25 JawSidewaysLeft · 26 JawSidewaysRight ·
27 JawThrust · 28 LidTightenerL · 29 LidTightenerR · 30 LipCornerDepressorL ·
31 LipCornerDepressorR · 32 LipCornerPullerL · 33 LipCornerPullerR ·
34 LipFunnelerLb · 35 LipFunnelerLt · 36 LipFunnelerRb · 37 LipFunnelerRt ·
38 LipPressorL · 39 LipPressorR · 40 LipPuckerL · 41 LipPuckerR ·
42 LipStretcherL · 43 LipStretcherR · 44 LipSuckLb · 45 LipSuckLt ·
46 LipSuckRb · 47 LipSuckRt · 48 LipTightenerL · 49 LipTightenerR ·
50 LipsToward · 51 LowerLipDepressorL · 52 LowerLipDepressorR · 53 MouthLeft ·
54 MouthRight · 55 NoseWrinklerL · 56 NoseWrinklerR · 57 OuterBrowRaiserL ·
58 OuterBrowRaiserR · 59 UpperLidRaiserL · 60 UpperLidRaiserR ·
61 UpperLipRaiserL · 62 UpperLipRaiserR · 63 TongueTipInterdental ·
64 TongueTipAlveolar · 65 TongueFrontDorsalPalate · 66 TongueMidDorsalPalate ·
67 TongueBackDorsalVelar · 68 TongueOut · 69 TongueRetreat

14..21（EyesLook\*）、69（TongueRetreat）上游不消费；注视方向来自双眼 Pose。

## 无版本号 → 防御策略与漂移风险

这个结构**没有版本号字段，也没有 VRCFT 惯用的自增计数前缀**（映射头 4 字节实际是
`FaceFlags + bool + 2 填充`）。VD Streamer 一旦改字段顺序/增删字段，读侧会**静默**
把数据读歪。因此本模块的闸门（全部在 `vd_payload` 实现）：

1. **映射长度校验**：`SharedMemory::Open(name, sizeof(FaceState))` ——不足 360 字节
   直接拒绝并提示版本可能不匹配；`ReadAt` 越界即失败且不部分写出。
2. **半更新识别**：连续取两次快照逐字节 `memcmp`；不一致视为「撞上写一半的帧」，
   本帧跳过（无计数前缀可用，只能这样防御）。
3. **全零识别**：映射存在但字段全零 = 串流端在跑但没在端面捕，不提交，并把问题
   指向「头显面捕是否启用 / 转发是否打开」。
4. **合理性校验（漂移哨兵）**：四个布尔字节必须 0/1；权重必须落在 [-0.2, 1.2]
   （非有限的 NaN/Inf 例外，交给清洗）；`*EyeIsValid` 置位时对应四元数模长必须在
   0.5..1.5 附近。任何一条不过 → 整帧拒绝并限频告警「可能是 VD 版本改变了结构
   布局」。
5. **NaN 清洗**：权重/置信度的非有限值归零、裁剪进 [0,1]；valid 眼的姿态四元数
   非有限则直接拒帧（注视没有安全的默认值）。

**版本漂移风险**：上述校验能拦住大多数「读歪」，但**挡不住语义级变更**（字段位置
没变、含义变了）。上游仓库本身就是社区逆向的产物，VD 也不承诺该格式稳定，真机
验收前请把它当作 untested 依赖。

## 到 Unified Expressions 的映射（已确认自 TrackingModule.cs）

闸门 A = `FaceFlags` bit0（嘴/颊/鼻/舌组）；闸门 B = `IsEyeFollowingBlendshapesValid`
（眼睑/眉组 + 睁眼度）。

| VD 下标 → 目标形状 | 闸门 |
|---|---|
| 0 → BrowPinchLeft + BrowLowererLeft；1 → 右侧同名 | B |
| 22/23 → BrowInnerUpLeft/Right；57/58 → BrowOuterUpLeft/Right | B |
| 28/29 → EyeSquintLeft/Right；59/60 → EyeWideLeft/Right | B |
| 24/25/26/27 → JawOpen/JawLeft/JawRight/JawForward | A |
| 50 → MouthClosed；53 → MouthUpperLeft+MouthLowerLeft；54 → 右侧两个 | A |
| 32 → MouthCornerPullLeft+MouthCornerSlantLeft（Slant 已烘焙进 Puller）；33 → 右侧两个 | A |
| 30/31 → MouthFrownLeft/Right；51/52 → MouthLowerDownLeft/Right | A |
| max(0, 61−55) → MouthUpperUpLeft+MouthUpperDeepenLeft（鼻皱补偿 workaround）；max(0, 62−56) → 右侧两个 | A |
| 9 → MouthRaiserUpper；8 → MouthRaiserLower；10/11 → MouthDimpleL/R | A |
| 48/49 → MouthTightenerL/R；38/39 → MouthPressL/R；42/43 → MouthStretchL/R | A |
| 40 → LipPuckerUpperLeft+LipPuckerLowerLeft；41 → 右侧两个 | A |
| 35/37 → LipFunnelUpperLeft/Right；34/36 → LipFunnelLowerLeft/Right | A |
| min(1−61^(1/6), 45) → LipSuckUpperLeft；min(1−62^(1/6), 47) → LipSuckUpperRight；44/46 → LipSuckLowerL/R | A |
| 2/3 → CheekPuffLeft/Right；6/7 → CheekSuckLeft/Right；4/5 → CheekSquintLeft/Right | A |
| 55/56 → NoseSneerLeft/Right | A |
| 68 → TongueOut；64 → TongueCurlUp（bit1 **未**置位） | A |
| 63→TongueOut、64→TongueLeft、65→TongueRight、66→TongueUp、67→TongueDown（bit1 置位时；与枚举名矛盾，语义**未确认**，照抄上游） | A |

睁眼度（已确认公式）：`openness = 1 − clamp(EyesClosed + EyesClosed × LidTightener, 0, 1)`，
逐眼用 12/13 与 28/29。

注视方向：眼姿态四元数按 `v = q·(0,0,−1)` 转成 HMD 局部单位向量（与 ALVR 模块同口径；
上游则先转 pitch/yaw 再交 VRCFT，最终语义一致——该等价性**推导确认**，极化待真机复核）。
`Left/RightEyeConfidence` 作为该眼 `gaze_confidence`（按字段名推断，未确认）。
逐眼只在 `Left/RightEyeIsValid` 分别置位时才写该眼注视位。

## 有意不照抄的上游行为

- `(_eyeAvailable && LeftEyeIsValid || RightEyeIsValid)` 是 `||` 优先级笔误：右眼有效
  会绕过眼动开关并连坐双眼。我们逐眼判定、互不冒名。
- 上游把瞳孔写死 5mm（伪造值）：我们不写，也不置 PUPIL 位。
- Pose 的 `Position`、`ExpressionConfidences[2]`、头姿：语义未确认，一律不消费。
- FaceFlags bit1 的舌头槽位复用（63..67 当 Out/Left/Right/Up/Down）与枚举名矛盾：
  照抄上游行为、不做发明，等待真机证据。
