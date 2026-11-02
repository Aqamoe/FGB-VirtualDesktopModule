// Virtual Desktop → FaceGaze Bridge 原生模块。
//
// 读取 VD 串流端（≥1.30）写的命名共享内存 VirtualDesktop.BodyState（FaceState 在
// 映射偏移 0），由命名事件 VirtualDesktop.BodyStateEvent 提示新帧，把 70 个表情
// 权重映射成 Unified Expressions，并输出逐眼睁眼度与注视方向。
// 不需要 VRCFT，不注入任何进程，也不需要管理员权限。
//
// 结构体没有版本号也没有计数前缀，防御策略（详见 docs/protocol.md）：
//   - 映射长度按 sizeof(FaceState) 校验，越界读一律拒绝（Kit SharedMemory 提供）；
//   - 连续两次快照逐字节比对识别「半更新」；
//   - 全零内容 → 「转发开了但面捕没数据」，不提交；
//   - 布尔字节 / 权重范围 / 单位四元数合理性校验兜底版本漂移。

#include "vd_payload.hpp"

#include "fgb/kit/module.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace {

constexpr std::uint64_t kNoDataTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;
constexpr std::uint64_t kWarnIntervalNs = 5ull * 1000ull * 1000ull * 1000ull;
// 映射/事件打不开时的重开间隔：VD 没运行就别每 tick 重开刷日志。
constexpr std::uint64_t kRetryIntervalNs = 2ull * 1000ull * 1000ull * 1000ull;

// 没有事件也能工作（内容轮询），但提交频率不能比这更高；
// 静置的脸会产生逐字节相同的快照，没有这一条会被误判成断流。
constexpr std::uint64_t kResubmitIntervalNs = 250ull * 1000ull * 1000ull;

// 事件等待超时：0 = 只探测不阻塞（update 在引擎线程里跑，绝不能睡）。
constexpr DWORD kEventPollTimeoutMs = 0;

class VirtualDesktopSource : public fgb::kit::IModuleSource {
public:
    bool Start(fgb::kit::Host& host, const FgbInitParams& /*params*/, std::string& error) override {
        fgb::kit::ShapeTable table;
        vd::DeclareTargets(table);
        if (!table.Build(host)) {
            error = "宿主没有提供 shape_index，无法映射 Unified Expressions";
            return false;
        }
        if (table.ResolvedCount() < table.Size()) {
            host.Warn("部分 Unified Expressions 形状未被宿主识别，已跳过 " +
                      std::to_string(table.Size() - table.ResolvedCount()) + " 个");
        }
        table_ = std::move(table);

        // 先试开一次；失败不致命——VD 可能还没启动，Read 里按间隔重试。
        std::string shm_error;
        if (!TryOpenState(host, shm_error)) {
            host.Info("启动时打不开 VirtualDesktop.BodyState（" + shm_error +
                      "），将按间隔重试；VD 串流端启动后自动接入");
        }
        host.StatusStarting("等待 Virtual Desktop 面捕数据");
        return true;
    }

    bool Read(fgb::kit::Host& host, FgbFrameSlot& slot, std::uint64_t now_ns) override {
        bool frame_written = false;
        if (shm_.Opened()) {
            frame_written = TryReadFrame(host, slot, now_ns);
        } else if (now_ns - last_retry_ns_ >= kRetryIntervalNs) {
            last_retry_ns_ = now_ns;
            std::string error;
            if (TryOpenState(host, error)) {
                host.Info("VirtualDesktop.BodyState 已出现，开始读取面捕帧");
                map_missing_reported_ = false;
            } else if (!map_missing_reported_) {
                // 只报一次：断流文案由 watchdog 统一给，避免每 tick 重开映射刷日志。
                map_missing_reported_ = true;
                host.Debug("打不开 VirtualDesktop.BodyState：" + error);
            }
        }
        watchdog_.Check(host, last_frame_ns_, now_ns);
        return frame_written;
    }

    void Stop(fgb::kit::Host& host) override {
        shm_.Close();
        event_.Close();
        host.StatusStopped("Virtual Desktop 已停止");
    }

private:
    bool TryOpenState(fgb::kit::Host& /*host*/, std::string& error) {
        bool ok = shm_.Open(vd::kStateMapName, sizeof(vd::FaceState), error);
        if (ok && !event_.IsOpen()) {
            std::string event_error;
            // 事件打不开不致命：Read 里有内容轮询兜底。
            if (!event_.Open(vd::kStateEventName, event_error)) {
                event_.Close();
            }
        }
        return ok;
    }

    bool TryReadFrame(fgb::kit::Host& host, FgbFrameSlot& slot, std::uint64_t now_ns) {
        const bool signaled = event_.IsOpen() && event_.Wait(kEventPollTimeoutMs);

        vd::FaceState snapshot{};
        if (!shm_.ReadAt(0, snapshot)) {
            // Open 已校验过长度，这里只防「映射在运行中被换小」的极端情况。
            host.WarnThrottled(torn_ns_, now_ns, kWarnIntervalNs,
                               "读取 VirtualDesktop.BodyState 越界，忽略本帧");
            return false;
        }

        vd::FaceState second{};
        if (!shm_.ReadAt(0, second) || std::memcmp(&snapshot, &second, sizeof(snapshot)) != 0) {
            // 两次快照不一致 = 大概率撞上写一半的帧；静置时逐字节相同不会误报。
            if (signaled) {
                host.WarnThrottled(torn_ns_, now_ns, kWarnIntervalNs,
                                   "VirtualDesktop.BodyState 疑似半更新，本帧跳过");
            }
            return false;
        }

        const vd::SnapshotKind kind = vd::Classify(snapshot);
        if (kind == vd::SnapshotKind::AllZero) {
            host.WarnThrottled(all_zero_ns_, now_ns, kWarnIntervalNs,
                               "VirtualDesktop.BodyState 内容全零：VD 串流端在运行但没在发面捕数据，"
                               "确认头显侧已启用面捕、串流端已开启面部追踪转发");
            return false;
        }
        if (kind == vd::SnapshotKind::Invalid) {
            host.WarnThrottled(invalid_ns_, now_ns, kWarnIntervalNs,
                               "VirtualDesktop.BodyState 数值校验失败：字段越界或姿态异常，"
                               "可能是 VD 版本改变了结构布局，本帧拒绝");
            return false;
        }

        vd::SanitizeInPlace(snapshot);
        const bool changed = !has_last_ || std::memcmp(&snapshot, &last_, sizeof(snapshot)) != 0;
        const bool heartbeat = now_ns - last_frame_ns_ >= kResubmitIntervalNs;
        if (!signaled && !changed && !heartbeat) {
            return false;  // 静置的脸：不重复提交，也不算断流（心跳兜底在下面）
        }

        if (!vd::WriteFrame(snapshot, table_, &slot)) {
            return false;
        }
        last_ = snapshot;
        has_last_ = true;
        last_frame_ns_ = now_ns;
        return true;
    }

    fgb::kit::SharedMemory shm_;
    fgb::kit::NamedEvent event_;
    fgb::kit::ShapeTable table_;
    vd::StreamWatchdog watchdog_{kNoDataTimeoutNs};
    vd::FaceState last_{};
    bool has_last_ = false;
    std::uint64_t last_frame_ns_ = 0;
    std::uint64_t last_retry_ns_ = 0;
    std::uint64_t torn_ns_ = 0;
    std::uint64_t all_zero_ns_ = 0;
    std::uint64_t invalid_ns_ = 0;
    bool map_missing_reported_ = false;
};

} // namespace

FGB_KIT_EXPORT(VirtualDesktopSource, "com.fgbbridge.virtual-desktop", "Virtual Desktop",
               FGB_CAP_EXPRESSION | FGB_CAP_EYES | FGB_CAP_GAZE)
