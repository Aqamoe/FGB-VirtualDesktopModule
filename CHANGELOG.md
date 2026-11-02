# 更新日志

遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与语义化版本。

## [未发布]

## [0.1.0] - 未发布
### 新增
- 读取 VD 串流端命名共享内存 `VirtualDesktop.BodyState`（FaceState，360 字节，
  偏移一手核对）+ 事件 `VirtualDesktop.BodyStateEvent`，输出 Unified Expressions
  表情、逐眼睁眼度与注视方向。不需要 VRCFT，不注入进程。
- 70 权重 → 69 个 Unified 目标逐项映射，含上游的睁眼度、上唇减鼻皱、吸唇幂次
  三个公式与 FaceFlags bit1 的舌头槽位行为。
- 无版本号结构的防御式读取：映射长度校验 + 越界拒绝、双快照半更新识别、
  全零识别、合理性漂移哨兵、NaN 清洗；断流文案指向「面部追踪转发是否开启」。
- 协议单测（布局/逐项映射/闸门/越界/清洗/断流）+ 真实 ABI 冒烟（自建同名映射
  写合成帧驱动提交校验）。

### 已知限制
- 未在真机（VD Streamer ≥1.30 + 带头显）验证，状态为未验证。
- 眼姿坐标系、舌头槽位语义、置信度字段含义均未确认，详见 docs/protocol.md。
