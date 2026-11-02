#pragma once
// Virtual Desktop 面捕共享内存的结构镜像、映射与断流监控。
//
// 上游（https://github.com/guygodin/VirtualDesktop.VRCFaceTracking，C#/MIT）由
// VD 串流端写命名共享内存 VirtualDesktop.BodyState，本文件按 FaceState.cs 的
// [StructLayout(LayoutKind.Sequential)] 逐字段镜像其内存布局（偏移已一手核对，
// 见 docs/protocol.md）。结构体没有版本号、也没有计数前缀，所以：
//   1. sizeof 与每个 offsetof 都有 static_assert + 运行期断言，改错就编译/测试失败；
//   2. 读取用 Kit 的 SharedMemory：映射长度先校验，越界即拒绝；
//   3. 半更新帧靠「连续两次快照逐字节比对」识别（上游无计数，只能这样防御）。
//
// 注意：MSVC 14.36 对「lambda 捕获数组后在体内下标访问」有 ICE，
// 本仓库相关代码一律用自由函数 + 显式引用参数，不要改回 lambda。

#include "fgb/kit/host.hpp"
#include "fgb/kit/shm.hpp"

#include <cstddef>
#include <cstdint>

namespace vd {

// ---- 上游常量（一手核对自 TrackingModule.cs 的字符串字面量） ----
// .NET 把无前缀名字原样交给 OpenFileMapping/OpenEvent，系统按「当前会话的
// Local 命名空间」解析；VD Streamer 与 FGB 都在用户桌面会话，所以这里必须
// 用同一张「裸名」，不要自作主张加 Global\ 或 Local\ 前缀。
inline constexpr wchar_t kStateMapName[] = L"VirtualDesktop.BodyState";
inline constexpr wchar_t kStateEventName[] = L"VirtualDesktop.BodyStateEvent";

// FaceFlags 位（上游只用到了低两位，其余位语义未确认）：
//   bit0 = 表情权重可用（嘴/颊/鼻/舌整组都以它为闸门）；
//   bit1 = 舌头取用的权重槽切换（详见 docs/protocol.md 的「未确认」）。
inline constexpr std::uint8_t kFaceFlagExpressions = 0b0001;
inline constexpr std::uint8_t kFaceFlagTongueAlt = 0b0010;

// ---- 70 个表情权重的下标（核对自 Expressions.cs，顺序即内存顺序） ----
// 这里只列映射表用到的常量；EyesLook*（14..21）上游不使用，
// 眼动方向来自双眼 Pose，而不是这些权重。
enum Expression : std::size_t {
    kExprBrowLowererL = 0,
    kExprBrowLowererR = 1,
    kExprCheekPuffL = 2,
    kExprCheekPuffR = 3,
    kExprCheekRaiserL = 4,
    kExprCheekRaiserR = 5,
    kExprCheekSuckL = 6,
    kExprCheekSuckR = 7,
    kExprChinRaiserB = 8,
    kExprChinRaiserT = 9,
    kExprDimplerL = 10,
    kExprDimplerR = 11,
    kExprEyesClosedL = 12,
    kExprEyesClosedR = 13,
    kExprInnerBrowRaiserL = 22,
    kExprInnerBrowRaiserR = 23,
    kExprJawDrop = 24,
    kExprJawSidewaysLeft = 25,
    kExprJawSidewaysRight = 26,
    kExprJawThrust = 27,
    kExprLidTightenerL = 28,
    kExprLidTightenerR = 29,
    kExprLipCornerDepressorL = 30,
    kExprLipCornerDepressorR = 31,
    kExprLipCornerPullerL = 32,
    kExprLipCornerPullerR = 33,
    kExprLipFunnelerLb = 34,
    kExprLipFunnelerLt = 35,
    kExprLipFunnelerRb = 36,
    kExprLipFunnelerRt = 37,
    kExprLipPressorL = 38,
    kExprLipPressorR = 39,
    kExprLipPuckerL = 40,
    kExprLipPuckerR = 41,
    kExprLipStretcherL = 42,
    kExprLipStretcherR = 43,
    kExprLipSuckLb = 44,
    kExprLipSuckLt = 45,
    kExprLipSuckRb = 46,
    kExprLipSuckRt = 47,
    kExprLipTightenerL = 48,
    kExprLipTightenerR = 49,
    kExprLipsToward = 50,
    kExprLowerLipDepressorL = 51,
    kExprLowerLipDepressorR = 52,
    kExprMouthLeft = 53,
    kExprMouthRight = 54,
    kExprNoseWrinklerL = 55,
    kExprNoseWrinklerR = 56,
    kExprOuterBrowRaiserL = 57,
    kExprOuterBrowRaiserR = 58,
    kExprUpperLidRaiserL = 59,
    kExprUpperLidRaiserR = 60,
    kExprUpperLipRaiserL = 61,
    kExprUpperLipRaiserR = 62,
    // 63..67 在 FaceFlags bit1 置位时被上游挪作 TongueOut/Left/Right/Up/Down，
    // 未置位时 64 = TongueTipAlveolar（喂 TongueCurlUp）、68 = TongueOut。
    kExprTongueSlot63 = 63,
    kExprTongueTipAlveolar = 64,
    kExprTongueSlot65 = 65,
    kExprTongueSlot66 = 66,
    kExprTongueSlot67 = 67,
    kExprTongueOut = 68,
    kExpressionCount = 70,
};

// ---- 线结构镜像（与 C# FaceState 逐字节一致，共 360 字节） ----
struct Vec3F {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct QuatF {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

// 上游 Pose = Orientation 在前、Position 在后（核对自 Pose.cs）。
struct PoseT {
    QuatF orientation;
    Vec3F position;
};

struct FaceState {
    std::uint8_t face_flags = 0;        // 0
    std::uint8_t eye_following_valid = 0; // 1（C# Marshal As I1，占 1 字节）
    std::uint8_t pad0[2] = {};          // 2..3（4 字节对齐补齐）
    float weights[kExpressionCount] = {}; // 4..283
    float expression_confidences[2] = {}; // 284..291（语义未确认，不消费）
    std::uint8_t left_eye_valid = 0;    // 292
    std::uint8_t right_eye_valid = 0;   // 293
    std::uint8_t pad1[2] = {};          // 294..295
    PoseT left_eye_pose;                // 296..323
    PoseT right_eye_pose;               // 324..351
    float left_eye_confidence = 0.0f;   // 352..355
    float right_eye_confidence = 0.0f;  // 356..359
};

// 一手核对的布局常量：任何改动都必须先过这些断言（测试里还有运行期复核）。
static_assert(sizeof(Vec3F) == 12);
static_assert(sizeof(QuatF) == 16);
static_assert(sizeof(PoseT) == 28);
static_assert(sizeof(FaceState) == 360);
static_assert(offsetof(FaceState, face_flags) == 0);
static_assert(offsetof(FaceState, eye_following_valid) == 1);
static_assert(offsetof(FaceState, weights) == 4);
static_assert(offsetof(FaceState, expression_confidences) == 284);
static_assert(offsetof(FaceState, left_eye_valid) == 292);
static_assert(offsetof(FaceState, right_eye_valid) == 293);
static_assert(offsetof(FaceState, left_eye_pose) == 296);
static_assert(offsetof(FaceState, right_eye_pose) == 324);
static_assert(offsetof(FaceState, left_eye_confidence) == 352);
static_assert(offsetof(FaceState, right_eye_confidence) == 356);

// 上游全部字段都是 1/4 字节类型，Pack 取 1/4/8 结果都一样是 360；
// 断言这一事实，防止「以为有对齐差异」。
static_assert(alignof(FaceState) == 4);

// ---- 帧内容分类与清洗 ----
enum class SnapshotKind {
    AllZero,  // 映射存在但整块全零：串流端没在端面捕数据
    Invalid,  // 布尔字节不是 0/1、权重越界太多、姿态非单位四元数 → 疑似版本漂移
    Usable,   // 通过校验，可以映射
};

// 纯校验，不修改 state。半更新/版本漂移防护的第一道闸。
SnapshotKind Classify(const FaceState& state);

// 就地把非有限的浮点替换成 0、把权重裁剪进 [0,1]、置信度裁剪进 [0,1]。
// 在 Classify 通过后调用，剩余的小越界（1.0001 之类）不再算异常。
void SanitizeInPlace(FaceState& state);

// ---- 映射 ----
// 登记本模块会写的所有 Unified Expressions 目标名。
void DeclareTargets(fgb::kit::ShapeTable& table);

// 按上游口径写一帧（调用方保证 state 已 Sanitize）：
//   FaceFlags bit0 未置位 → 不写表情、不置 FGB_ATTR_EXPRESSION；
//   IsEyeFollowingBlendshapesValid=0 → 不写眼睑/睁眼度；
//   逐眼 valid 才置该眼的注视位（上游的 || 优先级 bug 不照抄）。
// 返回是否写了至少一个通道。
bool WriteFrame(const FaceState& state, const fgb::kit::ShapeTable& table, FgbFrameSlot* slot);

// ---- 断流监控 ----
// 与 Kit 的 Staleness 同一套「只报一次、恢复自动回到活跃」的语义，
// 但文案改成 VD 专属：指向「串流端是否开启面部追踪转发、头显是否启用面捕」。
class StreamWatchdog {
public:
    explicit StreamWatchdog(std::uint64_t timeout_ns) : timeout_ns_{timeout_ns} {}

    // last_frame_ns == 0 表示从未拿到过可用帧。返回当前是否健康。
    bool Check(fgb::kit::Host& host, std::uint64_t last_frame_ns, std::uint64_t now_ns);
    bool Healthy() const { return healthy_; }

private:
    std::uint64_t timeout_ns_ = 0;
    bool healthy_ = false;
    bool reported_ = false;
};

} // namespace vd
