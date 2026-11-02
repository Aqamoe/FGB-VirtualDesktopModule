# 真机验收清单

> 当前状态：**未验证**。下列任一项完成后，请在 issue 里附头显型号与 VD 版本信息。

## 环境

| 项 | 值 |
|---|---|
| 头显 | 支持眼动的型号（Quest Pro 等；无眼动头显只应看到表情通道活跃） |
| VD Streamer | ≥1.30（记录版本号） |
| 系统 | Windows 10 / 11 x64 |
| 宿主 | FaceGaze Bridge（记录版本号） |

## 步骤

- [ ] PC 安装并运行 VD Streamer，串流设置开启「Forward tracking data to PC」
- [ ] 头显 VD 设置里启用面部追踪/面捕，按 VD 文档启动面捕会话
- [ ] PC 上关闭 VRCFT（其 Virtual Desktop 模块读同一命名对象）
- [ ] FGB「模块库 → 原生模块」安装 `virtual-desktop-<版本>.zip`
- [ ] 「模块」页把 Virtual Desktop 设为启动，重启引擎，戴上头显开始串流
- [ ] 模块卡片显示活跃，状态页有表情输入

## 观察项

- [ ] 张嘴 → `JawOpen`；微笑 → `MouthCornerPullLeft/Right`（含 Slant 同源值）
- [ ] 眨眼 → 对应眼 `openness` 下降（叠加 LidTightener 时更低），左右眼独立
- [ ] 眼球转动 → 注视方向跟随，且**只在该眼 valid 时更新**（另一眼不连坐）
- [ ] 极化核对：向上看时 `gaze_dir[1] > 0`，向右看时 `gaze_dir[0] > 0`（-Z 前）——
      这是 OpenXR 约定推断中最需要人眼确认的一项
- [ ] 摘下头显/停止串流 → 10 秒内卡片给出「已 N 秒没有更新」并指向转发开关；
      恢复串流后自动回到活跃
- [ ] 只启动 Streamer、不在头显端面捕 → 日志出现「内容全零」提示而不是静默
- [ ] 舌头（若头显支持）：bit1 行为按上游照抄，出现明显串位请附日志反馈

## 与上游对比（可选但有用）

同一台机器并用 VRCFT 的 Virtual Desktop 模块（v1.5），对比同一组表情的曲线与
注视极化。若 VRCFT 能动而本模块提示校验失败，多半是结构漂移哨兵过严或 VD 已改版，
请把 VD Streamer 版本号与两条日志一并提到 issue。
