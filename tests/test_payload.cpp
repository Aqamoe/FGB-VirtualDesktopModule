#include "vd_payload.hpp"

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using namespace fgb::kit;
using vd::FaceState;

namespace {

struct Expect {
    const char* name;
    float value;
};

FgbFrameSlot FreshSlot() {
    FgbFrameSlot slot{};
    slot.struct_size = sizeof(FgbFrameSlot);
    slot.abi_version = FGB_MODULE_ABI_V2;
    slot.shape_count = FGB_MAX_SHAPES;
    return slot;
}

// 每个下标一个唯一值：任何「映射接错线」都会让某个形状拿到别的下标的值。
float UniqueWeight(std::size_t i) {
    return static_cast<float>(i + 1) / static_cast<float>(vd::kExpressionCount + 1);
}

// 眼睑/眉组走 IsEyeFollowingBlendshapesValid 闸门，其余走 FaceFlags bit0。
bool IsEyeGateIndex(std::size_t i) {
    if (i <= 1) {
        return true;  // BrowLowerer L/R
    }
    if (i >= 22 && i <= 23) {
        return true;  // InnerBrowRaiser
    }
    if (i >= 28 && i <= 29) {
        return true;  // LidTightener
    }
    if (i >= 57 && i <= 60) {
        return true;  // OuterBrowRaiser / UpperLidRaiser
    }
    return i == 12 || i == 13;  // EyesClosed 只进睁眼度公式
}

std::size_t AddTarget(Expect* out, std::size_t count, const char* name, float value) {
    out[count].name = name;
    out[count].value = value;
    return count + 1;
}

// 逐下标期望表（口径 = docs/protocol.md 的上游映射，一手核对 TrackingModule.cs）。
std::size_t FillExpected(std::size_t i, float v, Expect* out) {
    std::size_t n = 0;
    switch (i) {
        case 0: n = AddTarget(out, n, "BrowPinchLeft", v); n = AddTarget(out, n, "BrowLowererLeft", v); break;
        case 1: n = AddTarget(out, n, "BrowPinchRight", v); n = AddTarget(out, n, "BrowLowererRight", v); break;
        case 2: n = AddTarget(out, n, "CheekPuffLeft", v); break;
        case 3: n = AddTarget(out, n, "CheekPuffRight", v); break;
        case 4: n = AddTarget(out, n, "CheekSquintLeft", v); break;
        case 5: n = AddTarget(out, n, "CheekSquintRight", v); break;
        case 6: n = AddTarget(out, n, "CheekSuckLeft", v); break;
        case 7: n = AddTarget(out, n, "CheekSuckRight", v); break;
        case 8: n = AddTarget(out, n, "MouthRaiserLower", v); break;
        case 9: n = AddTarget(out, n, "MouthRaiserUpper", v); break;
        case 10: n = AddTarget(out, n, "MouthDimpleLeft", v); break;
        case 11: n = AddTarget(out, n, "MouthDimpleRight", v); break;
        case 22: n = AddTarget(out, n, "BrowInnerUpLeft", v); break;
        case 23: n = AddTarget(out, n, "BrowInnerUpRight", v); break;
        case 24: n = AddTarget(out, n, "JawOpen", v); break;
        case 25: n = AddTarget(out, n, "JawLeft", v); break;
        case 26: n = AddTarget(out, n, "JawRight", v); break;
        case 27: n = AddTarget(out, n, "JawForward", v); break;
        case 28: n = AddTarget(out, n, "EyeSquintLeft", v); break;
        case 29: n = AddTarget(out, n, "EyeSquintRight", v); break;
        case 30: n = AddTarget(out, n, "MouthFrownLeft", v); break;
        case 31: n = AddTarget(out, n, "MouthFrownRight", v); break;
        case 32: n = AddTarget(out, n, "MouthCornerPullLeft", v); n = AddTarget(out, n, "MouthCornerSlantLeft", v); break;
        case 33: n = AddTarget(out, n, "MouthCornerPullRight", v); n = AddTarget(out, n, "MouthCornerSlantRight", v); break;
        case 34: n = AddTarget(out, n, "LipFunnelLowerLeft", v); break;
        case 35: n = AddTarget(out, n, "LipFunnelUpperLeft", v); break;
        case 36: n = AddTarget(out, n, "LipFunnelLowerRight", v); break;
        case 37: n = AddTarget(out, n, "LipFunnelUpperRight", v); break;
        case 38: n = AddTarget(out, n, "MouthPressLeft", v); break;
        case 39: n = AddTarget(out, n, "MouthPressRight", v); break;
        case 40: n = AddTarget(out, n, "LipPuckerUpperLeft", v); n = AddTarget(out, n, "LipPuckerLowerLeft", v); break;
        case 41: n = AddTarget(out, n, "LipPuckerUpperRight", v); n = AddTarget(out, n, "LipPuckerLowerRight", v); break;
        case 42: n = AddTarget(out, n, "MouthStretchLeft", v); break;
        case 43: n = AddTarget(out, n, "MouthStretchRight", v); break;
        case 44: n = AddTarget(out, n, "LipSuckLowerLeft", v); break;
        case 45: n = AddTarget(out, n, "LipSuckUpperLeft", v); break;  // min(1-0^(1/6)=1, v)
        case 46: n = AddTarget(out, n, "LipSuckLowerRight", v); break;
        case 47: n = AddTarget(out, n, "LipSuckUpperRight", v); break;
        case 48: n = AddTarget(out, n, "MouthTightenerLeft", v); break;
        case 49: n = AddTarget(out, n, "MouthTightenerRight", v); break;
        case 50: n = AddTarget(out, n, "MouthClosed", v); break;
        case 51: n = AddTarget(out, n, "MouthLowerDownLeft", v); break;
        case 52: n = AddTarget(out, n, "MouthLowerDownRight", v); break;
        case 53: n = AddTarget(out, n, "MouthUpperLeft", v); n = AddTarget(out, n, "MouthLowerLeft", v); break;
        case 54: n = AddTarget(out, n, "MouthUpperRight", v); n = AddTarget(out, n, "MouthLowerRight", v); break;
        case 55:
            n = AddTarget(out, n, "NoseSneerLeft", v);
            n = AddTarget(out, n, "MouthUpperUpLeft", 0.0f);      // max(0, 0 - v)
            n = AddTarget(out, n, "MouthUpperDeepenLeft", 0.0f);  // 同源
            break;
        case 56:
            n = AddTarget(out, n, "NoseSneerRight", v);
            n = AddTarget(out, n, "MouthUpperUpRight", 0.0f);
            n = AddTarget(out, n, "MouthUpperDeepenRight", 0.0f);
            break;
        case 57: n = AddTarget(out, n, "BrowOuterUpLeft", v); break;
        case 58: n = AddTarget(out, n, "BrowOuterUpRight", v); break;
        case 59: n = AddTarget(out, n, "EyeWideLeft", v); break;
        case 60: n = AddTarget(out, n, "EyeWideRight", v); break;
        case 61:
            n = AddTarget(out, n, "MouthUpperUpLeft", v);
            n = AddTarget(out, n, "MouthUpperDeepenLeft", v);
            n = AddTarget(out, n, "LipSuckUpperLeft", 0.0f);  // min(1-v^(1/6), 0)
            break;
        case 62:
            n = AddTarget(out, n, "MouthUpperUpRight", v);
            n = AddTarget(out, n, "MouthUpperDeepenRight", v);
            n = AddTarget(out, n, "LipSuckUpperRight", 0.0f);
            break;
        case 64: n = AddTarget(out, n, "TongueCurlUp", v); break;  // bit1 未置位
        case 68: n = AddTarget(out, n, "TongueOut", v); break;     // bit1 未置位
        default: break;  // 12/13/14..21/63/65..67/69：不产生形状写入
    }
    return n;
}

void CheckItemMapping(const ShapeTable& table, testing::HostMock& host, std::size_t i) {
    const float v = UniqueWeight(i);
    FaceState s{};
    if (IsEyeGateIndex(i)) {
        s.eye_following_valid = 1;
    } else {
        s.face_flags = vd::kFaceFlagExpressions;
    }
    s.weights[i] = v;

    FgbFrameSlot slot = FreshSlot();
    vd::WriteFrame(s, table, &slot);

    Expect exp[4];
    const std::size_t n = FillExpected(i, v, exp);
    for (std::size_t k = 0; k < n; ++k) {
        const std::int32_t index = host.IndexOf(exp[k].name);
        CHECK(index >= 0);
        if (index >= 0) {
            CHECK_EQ(slot.shape_valid[index], std::uint8_t{1});
            CHECK_NEAR(slot.shapes[index], exp[k].value, 2e-6);
        }
    }
    if (n == 0) {
        // 未被使用的下标（EyesLook*、TongueRetreat、bit1 未置位的 63/65/66/67）
        // 不允许把值「泄漏」到任何形状上。
        for (std::size_t k = 0; k < FGB_MAX_SHAPES; ++k) {
            if (slot.shape_valid[k] != 0) {
                CHECK(!(std::fabs(slot.shapes[k] - v) < 1e-6));
            }
        }
    }
    if (i == 12) {
        CHECK((slot.attributes & FGB_ATTR_LEFT_OPENNESS) != 0);
        CHECK_NEAR(slot.left_eye.openness, 1.0f - v, 2e-6);
        CHECK_NEAR(slot.right_eye.openness, 1.0f, 2e-6);
    }
    if (i == 13) {
        CHECK((slot.attributes & FGB_ATTR_RIGHT_OPENNESS) != 0);
        CHECK_NEAR(slot.right_eye.openness, 1.0f - v, 2e-6);
        CHECK_NEAR(slot.left_eye.openness, 1.0f, 2e-6);
    }
    // 本组测试的帧没有眼动姿态：注视位必须保持干净。
    CHECK((slot.attributes & (FGB_ATTR_LEFT_GAZE | FGB_ATTR_RIGHT_GAZE)) == 0);
}

// 一个「VD 正在面捕」的最小合法帧：布尔位规范、双眼单位四元数、权重有限。
FaceState MakeLiveFrame() {
    FaceState s{};
    s.face_flags = vd::kFaceFlagExpressions;
    s.eye_following_valid = 1;
    s.left_eye_valid = 1;
    s.right_eye_valid = 1;
    s.left_eye_pose.orientation.w = 1.0f;
    s.right_eye_pose.orientation.w = 1.0f;
    s.left_eye_confidence = 0.8f;
    s.right_eye_confidence = 0.7f;
    s.weights[vd::kExprJawDrop] = 0.62f;
    return s;
}

constexpr wchar_t kTestMapName[] = L"Local\\FGBVdUnitMapping";
constexpr std::size_t kTestMapBytes = 4096;

void CheckOutOfBoundReads() {
    std::string error;

    // 长度不足：映射存在但比结构体小（旧版/异常来源）必须拒绝，而不是盲读。
    {
        HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              static_cast<DWORD>(256), kTestMapName);
        CHECK(mapping != nullptr);
        if (mapping != nullptr) {
            SharedMemory small;
            // 页粒度会把 256 字节映射圆到 4096，所以这里用「结构体 16 倍」当门槛，
            // 验证的就是「按映射长度校验、越界即拒绝」这条路。
            CHECK(!small.Open(kTestMapName, sizeof(FaceState) * 16, error));
            CHECK(error.find("少于") != std::string::npos);
            ::CloseHandle(mapping);
        }
    }

    // 正常映射：整结构可读，贴边越界必须失败且不部分写出。
    {
        HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              static_cast<DWORD>(kTestMapBytes), kTestMapName);
        CHECK(mapping != nullptr);
        if (mapping == nullptr) {
            return;
        }
        auto* written = static_cast<std::uint8_t*>(::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, kTestMapBytes));
        CHECK(written != nullptr);
        if (written == nullptr) {
            ::CloseHandle(mapping);
            return;
        }
        const FaceState live = MakeLiveFrame();
        std::memcpy(written, &live, sizeof(live));

        SharedMemory reader;
        CHECK(reader.Open(kTestMapName, sizeof(FaceState), error));
        FaceState read{};
        CHECK(reader.ReadAt(0, read));
        CHECK_EQ(read.face_flags, std::uint8_t{vd::kFaceFlagExpressions});
        CHECK_NEAR(read.weights[vd::kExprJawDrop], 0.62, 1e-6);
        // 提醒：BodyState 头部是 face_flags + 补位 + weight[0]，不是自增计数，
        // 所以 Kit 的 Counter() 惯例在这里不可用——半更新只能靠双快照比对防御。

        // 越出映射尾部：ReadAt 返回 false 且出参保持原值（Kit 保证不部分写）。
        float tail = -123.0f;
        CHECK(!reader.ReadAt(kTestMapBytes - 2, tail));
        CHECK_NEAR(tail, -123.0, 1e-9);

        CHECK(reader.Size() >= sizeof(FaceState));
        reader.Close();
        ::UnmapViewOfFile(written);
        ::CloseHandle(mapping);
    }
}

void CheckWatchdog(testing::HostMock& host) {
    Host logger{host.services()};
    vd::StreamWatchdog watchdog{10ull * 1000000000ull};
    const std::uint64_t t0 = 1000000000ull;

    // 从未拿到帧：只报一次，且文案指向面捕转发。
    CHECK(!watchdog.Check(logger, 0, t0));
    CHECK(!watchdog.Check(logger, 0, t0 + 5000000000ull));
    std::size_t errors = 0;
    for (const auto& status : host.statuses()) {
        errors += status.first == FGB_STATUS_ERROR ? 1u : 0u;
    }
    CHECK_EQ(errors, static_cast<std::size_t>(1));
    bool mentions_forwarding = false;
    for (const auto& status : host.statuses()) {
        if (status.first == FGB_STATUS_ERROR &&
            status.second.find("转发") != std::string::npos) {
            mentions_forwarding = true;
        }
    }
    CHECK(mentions_forwarding);

    // 有帧且新鲜：恢复为活跃。
    CHECK(watchdog.Check(logger, t0 + 10000000000ull, t0 + 11000000000ull));
    CHECK(host.HasStatus(FGB_STATUS_ACTIVE));

    // 断流 11 秒：再报一次带秒数的错，不刷屏。
    const std::size_t before = host.CountStatuses(FGB_STATUS_ERROR);
    CHECK(!watchdog.Check(logger, t0 + 11000000000ull, t0 + 22000000000ull));
    CHECK(!watchdog.Check(logger, t0 + 11000000000ull, t0 + 30000000000ull));
    CHECK_EQ(host.CountStatuses(FGB_STATUS_ERROR), before + 1);
    const std::string last = host.LastStatusDetail();
    CHECK(last.find("秒没有更新") != std::string::npos);
    CHECK(last.find("Streamer") != std::string::npos);
}

} // namespace

int main() {
    // ---- 结构体布局：与一手核对的 C# 偏移逐项一致（static_assert 的运行时双保险） ----
    // 用非 const 局部数组承载，避免「常量条件表达式」告警；对照 docs/protocol.md 的表。
    std::size_t actual_offsets[10];
    actual_offsets[0] = offsetof(FaceState, face_flags);
    actual_offsets[1] = offsetof(FaceState, eye_following_valid);
    actual_offsets[2] = offsetof(FaceState, weights);
    actual_offsets[3] = offsetof(FaceState, expression_confidences);
    actual_offsets[4] = offsetof(FaceState, left_eye_valid);
    actual_offsets[5] = offsetof(FaceState, right_eye_valid);
    actual_offsets[6] = offsetof(FaceState, left_eye_pose);
    actual_offsets[7] = offsetof(FaceState, right_eye_pose);
    actual_offsets[8] = offsetof(FaceState, left_eye_confidence);
    actual_offsets[9] = offsetof(FaceState, right_eye_confidence);
    const std::size_t expected_offsets[10] = {0, 1, 4, 284, 292, 293, 296, 324, 352, 356};
    for (std::size_t i = 0; i < 10; ++i) {
        CHECK_EQ(actual_offsets[i], expected_offsets[i]);
    }
    std::size_t sizes[4];
    sizes[0] = sizeof(FaceState);
    sizes[1] = sizeof(vd::PoseT);
    sizes[2] = sizeof(vd::QuatF);
    sizes[3] = sizeof(vd::Vec3F);
    CHECK_EQ(sizes[0], static_cast<std::size_t>(360));
    CHECK_EQ(sizes[1], static_cast<std::size_t>(28));
    CHECK_EQ(sizes[2], static_cast<std::size_t>(16));
    CHECK_EQ(sizes[3], static_cast<std::size_t>(12));
    // Pose 内部顺序：Orientation 在前、Position 在后（上游 Pose.cs）
    std::size_t pose_offsets[2];
    pose_offsets[0] = offsetof(vd::PoseT, orientation);
    pose_offsets[1] = offsetof(vd::PoseT, position);
    CHECK_EQ(pose_offsets[0], static_cast<std::size_t>(0));
    CHECK_EQ(pose_offsets[1], static_cast<std::size_t>(16));
    std::size_t alignment = alignof(FaceState);
    CHECK_EQ(alignment, static_cast<std::size_t>(4));

    // ---- 70 个权重逐项映射 ----
    testing::HostMock host;
    Host logger{host.services()};
    ShapeTable table;
    vd::DeclareTargets(table);
    CHECK(table.Build(logger));
    CHECK_EQ(table.ResolvedCount(), table.Size());  // 目标名必须都是真实形状

    for (std::size_t i = 0; i < vd::kExpressionCount; ++i) {
        CheckItemMapping(table, host, i);
    }

    // ---- 闸门：未亮的通道不能置属性位 ----
    {
        FaceState s = MakeLiveFrame();
        s.face_flags = 0;  // bit0 未亮：嘴组整组不写
        s.eye_following_valid = 0;
        s.left_eye_valid = 0;
        s.right_eye_valid = 0;
        FgbFrameSlot slot = FreshSlot();
        CHECK(!vd::WriteFrame(s, table, &slot));
        CHECK((slot.attributes & FGB_ATTR_EXPRESSION) == 0);
        CHECK((slot.attributes & (FGB_ATTR_LEFT_OPENNESS | FGB_ATTR_RIGHT_OPENNESS)) == 0);
        CHECK((slot.attributes & (FGB_ATTR_LEFT_GAZE | FGB_ATTR_RIGHT_GAZE)) == 0);
        CHECK_EQ(slot.shape_valid[host.IndexOf("JawOpen")], std::uint8_t{0});
    }
    {
        // 只有 bit0：嘴组写、眼睑/眉组与睁眼度不写。
        FaceState s = MakeLiveFrame();
        s.eye_following_valid = 0;
        FgbFrameSlot slot = FreshSlot();
        CHECK(vd::WriteFrame(s, table, &slot));
        CHECK((slot.attributes & FGB_ATTR_EXPRESSION) != 0);
        CHECK_EQ(slot.shape_valid[host.IndexOf("JawOpen")], std::uint8_t{1});
        CHECK((slot.attributes & (FGB_ATTR_LEFT_OPENNESS | FGB_ATTR_RIGHT_OPENNESS)) == 0);
        CHECK_EQ(slot.shape_valid[host.IndexOf("EyeSquintLeft")], std::uint8_t{0});
        CHECK_EQ(slot.shape_valid[host.IndexOf("BrowInnerUpLeft")], std::uint8_t{0});
    }
    {
        // 单眼 valid：另一只眼的注视位不能被连坐（上游 || 优先级 bug 不照抄）。
        FaceState s = MakeLiveFrame();
        s.right_eye_valid = 0;
        FgbFrameSlot slot = FreshSlot();
        CHECK(vd::WriteFrame(s, table, &slot));
        CHECK((slot.attributes & FGB_ATTR_LEFT_GAZE) != 0);
        CHECK((slot.attributes & FGB_ATTR_RIGHT_GAZE) == 0);
        CHECK_NEAR(slot.left_eye.gaze_dir[2], -1.0, 1e-6);
        CHECK_NEAR(slot.left_eye.gaze_confidence, 0.8, 1e-6);
    }
    {
        // FaceFlags bit1：舌头改读 63..67，TongueCurlUp 不再写（上游行为照抄）。
        FaceState s = MakeLiveFrame();
        s.face_flags = static_cast<std::uint8_t>(vd::kFaceFlagExpressions | vd::kFaceFlagTongueAlt);
        s.weights[vd::kExprTongueSlot63] = 0.11f;
        s.weights[vd::kExprTongueTipAlveolar] = 0.22f;
        s.weights[vd::kExprTongueSlot65] = 0.33f;
        s.weights[vd::kExprTongueSlot66] = 0.44f;
        s.weights[vd::kExprTongueSlot67] = 0.55f;
        s.weights[vd::kExprTongueOut] = 0.99f;
        FgbFrameSlot slot = FreshSlot();
        CHECK(vd::WriteFrame(s, table, &slot));
        CHECK_NEAR(slot.shapes[host.IndexOf("TongueOut")], 0.11, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("TongueLeft")], 0.22, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("TongueRight")], 0.33, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("TongueUp")], 0.44, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("TongueDown")], 0.55, 1e-6);
        CHECK_EQ(slot.shape_valid[host.IndexOf("TongueCurlUp")], std::uint8_t{0});
    }

    // ---- 快照分类：全零 / 半更新 / 漂移 ----
    {
        FaceState zero{};
        CHECK(vd::Classify(zero) == vd::SnapshotKind::AllZero);
        CHECK(vd::Classify(MakeLiveFrame()) == vd::SnapshotKind::Usable);

        FaceState bad_bool = MakeLiveFrame();
        bad_bool.face_flags = 0x7b;  // 不是 0/1 组合能解释的字节，更像错位
        CHECK(vd::Classify(bad_bool) == vd::SnapshotKind::Invalid);

        FaceState bad_weight = MakeLiveFrame();
        bad_weight.weights[vd::kExprJawDrop] = 1250.0f;  // 远超范围 = 结构漂移
        CHECK(vd::Classify(bad_weight) == vd::SnapshotKind::Invalid);

        FaceState bad_pose = MakeLiveFrame();
        bad_pose.left_eye_pose.orientation.w = 0.0f;  // valid 位亮着却是零四元数
        CHECK(vd::Classify(bad_pose) == vd::SnapshotKind::Invalid);
    }

    // ---- NaN 清洗：权重 NaN → 0；置信度 NaN → 0；valid 眼的 NaN 四元数整帧拒绝 ----
    {
        FaceState s = MakeLiveFrame();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        s.weights[vd::kExprJawDrop] = nan;
        s.weights[vd::kExprLipCornerPullerL] = std::numeric_limits<float>::infinity();
        s.left_eye_confidence = nan;
        CHECK(vd::Classify(s) == vd::SnapshotKind::Usable);  // 非有限值归清洗管，不算漂移
        vd::SanitizeInPlace(s);
        FgbFrameSlot slot = FreshSlot();
        CHECK(vd::WriteFrame(s, table, &slot));
        CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.0, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("MouthCornerPullLeft")], 0.0, 1e-6);
        CHECK_NEAR(slot.left_eye.gaze_confidence, 0.0, 1e-6);
        CHECK(std::isfinite(slot.left_eye.gaze_dir[0]));

        FaceState pose_nan = MakeLiveFrame();
        pose_nan.left_eye_pose.orientation.x = std::numeric_limits<float>::quiet_NaN();
        CHECK(vd::Classify(pose_nan) == vd::SnapshotKind::Invalid);  // 注视没有安全默认值，拒帧
    }

    // ---- 越界读拒绝（真实映射） ----
    CheckOutOfBoundReads();

    // ---- 断流 ----
    CheckWatchdog(host);

    return CheckSummary("virtual-desktop-payload");
}
