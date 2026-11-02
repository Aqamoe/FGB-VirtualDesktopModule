#include "vd_payload.hpp"

#include <cmath>
#include <cstring>

namespace vd {

namespace {

bool IsByteFlag(std::uint8_t value) {
    return value == 0 || value == 1;
}

bool IsFinite(float value) {
    return std::isfinite(value);
}

// 上游的权重都应在 [0,1]；留一点容差，越界太远（比如整段错位读到的大数）
// 视为「结构漂移」而不是「小幅超范围」。非有限值（NaN/Inf）单独走清洗：
// 它们更像半更新瞬间的撕裂，不该把整帧判死。
bool WeightPlausible(float value) {
    if (!IsFinite(value)) {
        return true;
    }
    return value >= -0.2f && value <= 1.2f;
}

// 单位四元数合理性：valid 位置位时，对应姿态必须是接近单位的四元数。
// 全零/半零是「没人写过这块」或「布局读歪了」的典型形态，按 Invalid 处理；
// 清洗一个 NaN 四元数不如直接拒帧来得诚实（注视方向没有安全默认值）。
bool PosePlausible(const PoseT& pose, bool valid) {
    if (!valid) {
        return true;
    }
    if (!IsFinite(pose.orientation.x) || !IsFinite(pose.orientation.y) ||
        !IsFinite(pose.orientation.z) || !IsFinite(pose.orientation.w)) {
        return false;
    }
    const float squared = pose.orientation.x * pose.orientation.x +
                          pose.orientation.y * pose.orientation.y +
                          pose.orientation.z * pose.orientation.z +
                          pose.orientation.w * pose.orientation.w;
    return squared > 0.25f && squared < 2.25f;
}

bool AllFieldsZero(const FaceState& s) {
    for (std::size_t i = 0; i < kExpressionCount; ++i) {
        if (s.weights[i] != 0.0f) {
            return false;
        }
    }
    return s.face_flags == 0 && s.eye_following_valid == 0 && s.left_eye_valid == 0 &&
           s.right_eye_valid == 0 && s.expression_confidences[0] == 0.0f &&
           s.expression_confidences[1] == 0.0f && s.left_eye_confidence == 0.0f &&
           s.right_eye_confidence == 0.0f &&
           s.left_eye_pose.orientation.x == 0.0f && s.left_eye_pose.orientation.y == 0.0f &&
           s.left_eye_pose.orientation.z == 0.0f && s.left_eye_pose.orientation.w == 0.0f &&
           s.right_eye_pose.orientation.x == 0.0f && s.right_eye_pose.orientation.y == 0.0f &&
           s.right_eye_pose.orientation.z == 0.0f && s.right_eye_pose.orientation.w == 0.0f &&
           s.left_eye_pose.position.x == 0.0f && s.right_eye_pose.position.x == 0.0f;
}

float Clean01(float value) {
    if (!IsFinite(value)) {
        return 0.0f;
    }
    return fgb::kit::Clamp01(value);
}

void CleanPoseInPlace(PoseT& pose) {
    float parts[7] = {pose.orientation.x, pose.orientation.y, pose.orientation.z,
                      pose.orientation.w, pose.position.x,      pose.position.y,
                      pose.position.z};
    for (std::size_t i = 0; i < 7; ++i) {
        if (!IsFinite(parts[i])) {
            parts[i] = 0.0f;
        }
    }
    pose.orientation.x = parts[0];
    pose.orientation.y = parts[1];
    pose.orientation.z = parts[2];
    pose.orientation.w = parts[3];
    pose.position.x = parts[4];
    pose.position.y = parts[5];
    pose.position.z = parts[6];
}

// 上游 Quaternion.Cartesian()：先归一化再取 pitch/yaw；
// 零四元数没有方向语义，返回 false，绝不伪造「看正前方」。
bool GazeDirFromQuat(const QuatF& q, float (&direction)[3]) {
    if (!IsFinite(q.x) || !IsFinite(q.y) || !IsFinite(q.z) || !IsFinite(q.w)) {
        return false;
    }
    const float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(norm > 0.001f)) {
        return false;
    }
    const float x = q.x / norm;
    const float y = q.y / norm;
    const float z = q.z / norm;
    const float w = q.w / norm;
    // v = q * (0,0,-1)：OpenXR 与 FGB ABI 同为右手系、-Z 前、+Y 上、+X 右
    // （与 ALVR 模块同口径；VD 头显侧眼动姿态即 OpenXR 约定）。
    direction[0] = -2.0f * (x * z + w * y);
    direction[1] = 2.0f * (x * w - y * z);
    direction[2] = 2.0f * (x * x + y * y) - 1.0f;
    return true;
}

// 上游睁眼度公式：1 - clamp(closed + closed * tightener, 0, 1)。
float Openness(float closed, float tightener) {
    const float penalty = closed + closed * tightener;
    return 1.0f - Clean01(penalty);
}

// ShapeTable::Write 收 string_view + float；这里包一层短名，避免整页重复。
void W(const fgb::kit::ShapeTable& table, FgbFrameSlot* slot, std::string_view name,
       const float* weights, std::size_t source) {
    table.Write(slot, name, weights[source]);
}

void Wf(const fgb::kit::ShapeTable& table, FgbFrameSlot* slot, std::string_view name,
        float value) {
    table.Write(slot, name, value);
}

} // namespace

SnapshotKind Classify(const FaceState& state) {
    if (AllFieldsZero(state)) {
        return SnapshotKind::AllZero;
    }
    if (!IsByteFlag(state.face_flags) || !IsByteFlag(state.eye_following_valid) ||
        !IsByteFlag(state.left_eye_valid) || !IsByteFlag(state.right_eye_valid)) {
        return SnapshotKind::Invalid;
    }
    for (std::size_t i = 0; i < kExpressionCount; ++i) {
        if (!WeightPlausible(state.weights[i])) {
            return SnapshotKind::Invalid;
        }
    }
    if (!PosePlausible(state.left_eye_pose, state.left_eye_valid != 0) ||
        !PosePlausible(state.right_eye_pose, state.right_eye_valid != 0)) {
        return SnapshotKind::Invalid;
    }
    return SnapshotKind::Usable;
}

void SanitizeInPlace(FaceState& state) {
    for (std::size_t i = 0; i < kExpressionCount; ++i) {
        state.weights[i] = Clean01(state.weights[i]);
    }
    state.expression_confidences[0] = Clean01(state.expression_confidences[0]);
    state.expression_confidences[1] = Clean01(state.expression_confidences[1]);
    state.left_eye_confidence = Clean01(state.left_eye_confidence);
    state.right_eye_confidence = Clean01(state.right_eye_confidence);
    CleanPoseInPlace(state.left_eye_pose);
    CleanPoseInPlace(state.right_eye_pose);
}

void DeclareTargets(fgb::kit::ShapeTable& table) {
    // 眼睑/眉（IsEyeFollowingBlendshapesValid 闸门）
    table.Declare("EyeWideLeft");
    table.Declare("EyeWideRight");
    table.Declare("EyeSquintLeft");
    table.Declare("EyeSquintRight");
    table.Declare("BrowInnerUpLeft");
    table.Declare("BrowInnerUpRight");
    table.Declare("BrowOuterUpLeft");
    table.Declare("BrowOuterUpRight");
    table.Declare("BrowPinchLeft");
    table.Declare("BrowPinchRight");
    table.Declare("BrowLowererLeft");
    table.Declare("BrowLowererRight");
    // 下颌/嘴/唇/颊/鼻/舌（FaceFlags bit0 闸门）
    table.Declare("JawOpen");
    table.Declare("JawLeft");
    table.Declare("JawRight");
    table.Declare("JawForward");
    table.Declare("MouthClosed");
    table.Declare("MouthUpperLeft");
    table.Declare("MouthLowerLeft");
    table.Declare("MouthUpperRight");
    table.Declare("MouthLowerRight");
    table.Declare("MouthCornerPullLeft");
    table.Declare("MouthCornerSlantLeft");
    table.Declare("MouthCornerPullRight");
    table.Declare("MouthCornerSlantRight");
    table.Declare("MouthFrownLeft");
    table.Declare("MouthFrownRight");
    table.Declare("MouthLowerDownLeft");
    table.Declare("MouthLowerDownRight");
    table.Declare("MouthUpperUpLeft");
    table.Declare("MouthUpperDeepenLeft");
    table.Declare("MouthUpperUpRight");
    table.Declare("MouthUpperDeepenRight");
    table.Declare("MouthRaiserUpper");
    table.Declare("MouthRaiserLower");
    table.Declare("MouthDimpleLeft");
    table.Declare("MouthDimpleRight");
    table.Declare("MouthTightenerLeft");
    table.Declare("MouthTightenerRight");
    table.Declare("MouthPressLeft");
    table.Declare("MouthPressRight");
    table.Declare("MouthStretchLeft");
    table.Declare("MouthStretchRight");
    table.Declare("LipPuckerUpperLeft");
    table.Declare("LipPuckerLowerLeft");
    table.Declare("LipPuckerUpperRight");
    table.Declare("LipPuckerLowerRight");
    table.Declare("LipFunnelUpperLeft");
    table.Declare("LipFunnelUpperRight");
    table.Declare("LipFunnelLowerLeft");
    table.Declare("LipFunnelLowerRight");
    table.Declare("LipSuckUpperLeft");
    table.Declare("LipSuckUpperRight");
    table.Declare("LipSuckLowerLeft");
    table.Declare("LipSuckLowerRight");
    table.Declare("CheekPuffLeft");
    table.Declare("CheekPuffRight");
    table.Declare("CheekSuckLeft");
    table.Declare("CheekSuckRight");
    table.Declare("CheekSquintLeft");
    table.Declare("CheekSquintRight");
    table.Declare("NoseSneerLeft");
    table.Declare("NoseSneerRight");
    table.Declare("TongueOut");
    table.Declare("TongueLeft");
    table.Declare("TongueRight");
    table.Declare("TongueUp");
    table.Declare("TongueDown");
    table.Declare("TongueCurlUp");
}

bool WriteFrame(const FaceState& state, const fgb::kit::ShapeTable& table, FgbFrameSlot* slot) {
    const float* e = state.weights;
    bool wrote_any = false;

    // ---- 眼睑/眉：上游以 IsEyeFollowingBlendshapesValid 为闸门 ----
    if (state.eye_following_valid != 0) {
        wrote_any = true;
        slot->attributes |= FGB_ATTR_LEFT_OPENNESS | FGB_ATTR_RIGHT_OPENNESS;
        slot->left_eye.openness = Openness(e[kExprEyesClosedL], e[kExprLidTightenerL]);
        slot->right_eye.openness = Openness(e[kExprEyesClosedR], e[kExprLidTightenerR]);
        // 上游没有逐眼睁眼置信度字段，来源是设备端识别，给满。
        slot->left_eye.openness_confidence = 1.0f;
        slot->right_eye.openness_confidence = 1.0f;

        W(table, slot, "EyeWideLeft", e, kExprUpperLidRaiserL);
        W(table, slot, "EyeWideRight", e, kExprUpperLidRaiserR);
        W(table, slot, "EyeSquintLeft", e, kExprLidTightenerL);
        W(table, slot, "EyeSquintRight", e, kExprLidTightenerR);
        W(table, slot, "BrowInnerUpLeft", e, kExprInnerBrowRaiserL);
        W(table, slot, "BrowInnerUpRight", e, kExprInnerBrowRaiserR);
        W(table, slot, "BrowOuterUpLeft", e, kExprOuterBrowRaiserL);
        W(table, slot, "BrowOuterUpRight", e, kExprOuterBrowRaiserR);
        // 上游把 BrowLowerer 同时喂给 Pinch 与 Lowerer 两个形状。
        W(table, slot, "BrowPinchLeft", e, kExprBrowLowererL);
        W(table, slot, "BrowLowererLeft", e, kExprBrowLowererL);
        W(table, slot, "BrowPinchRight", e, kExprBrowLowererR);
        W(table, slot, "BrowLowererRight", e, kExprBrowLowererR);
    }

    // ---- 嘴/颊/鼻/舌：上游以 FaceFlags bit0 为闸门 ----
    if ((state.face_flags & kFaceFlagExpressions) != 0) {
        wrote_any = true;
        W(table, slot, "JawOpen", e, kExprJawDrop);
        W(table, slot, "JawLeft", e, kExprJawSidewaysLeft);
        W(table, slot, "JawRight", e, kExprJawSidewaysRight);
        W(table, slot, "JawForward", e, kExprJawThrust);

        W(table, slot, "MouthClosed", e, kExprLipsToward);
        W(table, slot, "MouthUpperLeft", e, kExprMouthLeft);
        W(table, slot, "MouthLowerLeft", e, kExprMouthLeft);
        W(table, slot, "MouthUpperRight", e, kExprMouthRight);
        W(table, slot, "MouthLowerRight", e, kExprMouthRight);

        W(table, slot, "MouthCornerPullLeft", e, kExprLipCornerPullerL);
        W(table, slot, "MouthCornerSlantLeft", e, kExprLipCornerPullerL);
        W(table, slot, "MouthCornerPullRight", e, kExprLipCornerPullerR);
        W(table, slot, "MouthCornerSlantRight", e, kExprLipCornerPullerR);
        W(table, slot, "MouthFrownLeft", e, kExprLipCornerDepressorL);
        W(table, slot, "MouthFrownRight", e, kExprLipCornerDepressorR);
        W(table, slot, "MouthLowerDownLeft", e, kExprLowerLipDepressorL);
        W(table, slot, "MouthLowerDownRight", e, kExprLowerLipDepressorR);

        // 上游的「上抬唇减鼻皱」 workaround，防止上唇动作被鼻皱吃掉。
        const float upper_up_left = std::fmax(0.0f, e[kExprUpperLipRaiserL] - e[kExprNoseWrinklerL]);
        const float upper_up_right = std::fmax(0.0f, e[kExprUpperLipRaiserR] - e[kExprNoseWrinklerR]);
        Wf(table, slot, "MouthUpperUpLeft", upper_up_left);
        Wf(table, slot, "MouthUpperDeepenLeft", upper_up_left);
        Wf(table, slot, "MouthUpperUpRight", upper_up_right);
        Wf(table, slot, "MouthUpperDeepenRight", upper_up_right);

        W(table, slot, "MouthRaiserUpper", e, kExprChinRaiserT);
        W(table, slot, "MouthRaiserLower", e, kExprChinRaiserB);
        W(table, slot, "MouthDimpleLeft", e, kExprDimplerL);
        W(table, slot, "MouthDimpleRight", e, kExprDimplerR);
        W(table, slot, "MouthTightenerLeft", e, kExprLipTightenerL);
        W(table, slot, "MouthTightenerRight", e, kExprLipTightenerR);
        W(table, slot, "MouthPressLeft", e, kExprLipPressorL);
        W(table, slot, "MouthPressRight", e, kExprLipPressorR);
        W(table, slot, "MouthStretchLeft", e, kExprLipStretcherL);
        W(table, slot, "MouthStretchRight", e, kExprLipStretcherR);

        W(table, slot, "LipPuckerUpperLeft", e, kExprLipPuckerL);
        W(table, slot, "LipPuckerLowerLeft", e, kExprLipPuckerL);
        W(table, slot, "LipPuckerUpperRight", e, kExprLipPuckerR);
        W(table, slot, "LipPuckerLowerRight", e, kExprLipPuckerR);

        W(table, slot, "LipFunnelUpperLeft", e, kExprLipFunnelerLt);
        W(table, slot, "LipFunnelUpperRight", e, kExprLipFunnelerRt);
        W(table, slot, "LipFunnelLowerLeft", e, kExprLipFunnelerLb);
        W(table, slot, "LipFunnelLowerRight", e, kExprLipFunnelerRb);

        // 上游公式：min(1 - raiser^(1/6), suck)。1/6 次幂压住「上唇微抬就吸唇」的抖动。
        const float suck_upper_left =
            std::fmin(1.0f - std::powf(e[kExprUpperLipRaiserL], 1.0f / 6.0f), e[kExprLipSuckLt]);
        const float suck_upper_right =
            std::fmin(1.0f - std::powf(e[kExprUpperLipRaiserR], 1.0f / 6.0f), e[kExprLipSuckRt]);
        Wf(table, slot, "LipSuckUpperLeft", suck_upper_left);
        Wf(table, slot, "LipSuckUpperRight", suck_upper_right);
        W(table, slot, "LipSuckLowerLeft", e, kExprLipSuckLb);
        W(table, slot, "LipSuckLowerRight", e, kExprLipSuckRb);

        W(table, slot, "CheekPuffLeft", e, kExprCheekPuffL);
        W(table, slot, "CheekPuffRight", e, kExprCheekPuffR);
        W(table, slot, "CheekSuckLeft", e, kExprCheekSuckL);
        W(table, slot, "CheekSuckRight", e, kExprCheekSuckR);
        W(table, slot, "CheekSquintLeft", e, kExprCheekRaiserL);
        W(table, slot, "CheekSquintRight", e, kExprCheekRaiserR);

        W(table, slot, "NoseSneerLeft", e, kExprNoseWrinklerL);
        W(table, slot, "NoseSneerRight", e, kExprNoseWrinklerR);

        // 舌头：FaceFlags bit1 决定取哪批槽位（上游行为，语义未确认，照抄不发明）。
        if ((state.face_flags & kFaceFlagTongueAlt) != 0) {
            W(table, slot, "TongueOut", e, kExprTongueSlot63);
            W(table, slot, "TongueLeft", e, kExprTongueTipAlveolar);
            W(table, slot, "TongueRight", e, kExprTongueSlot65);
            W(table, slot, "TongueUp", e, kExprTongueSlot66);
            W(table, slot, "TongueDown", e, kExprTongueSlot67);
        } else {
            W(table, slot, "TongueOut", e, kExprTongueOut);
            W(table, slot, "TongueCurlUp", e, kExprTongueTipAlveolar);
        }
    }

    if (wrote_any) {
        slot->attributes |= FGB_ATTR_EXPRESSION;
    }

    // ---- 逐眼注视：上游把整个 Pose 组喂给双眼；我们逐眼按 valid 走，
    // 不把「另一只眼有效」冒名成这只眼有数据（ABI 是属性级有效）。----
    float direction[3] = {0.0f, 0.0f, -1.0f};
    if (state.left_eye_valid != 0 && GazeDirFromQuat(state.left_eye_pose.orientation, direction)) {
        slot->attributes |= FGB_ATTR_LEFT_GAZE;
        slot->left_eye.gaze_dir[0] = direction[0];
        slot->left_eye.gaze_dir[1] = direction[1];
        slot->left_eye.gaze_dir[2] = direction[2];
        slot->left_eye.gaze_confidence = state.left_eye_confidence;
        wrote_any = true;
    }
    if (state.right_eye_valid != 0 &&
        GazeDirFromQuat(state.right_eye_pose.orientation, direction)) {
        slot->attributes |= FGB_ATTR_RIGHT_GAZE;
        slot->right_eye.gaze_dir[0] = direction[0];
        slot->right_eye.gaze_dir[1] = direction[1];
        slot->right_eye.gaze_dir[2] = direction[2];
        slot->right_eye.gaze_confidence = state.right_eye_confidence;
        wrote_any = true;
    }
    // 上游写死瞳孔 5mm：那是伪造值，我们不写，也不置 PUPIL 位。
    return wrote_any;
}

bool StreamWatchdog::Check(fgb::kit::Host& host, std::uint64_t last_frame_ns,
                           std::uint64_t now_ns) {
    const bool fresh =
        last_frame_ns != 0 && now_ns >= last_frame_ns && now_ns - last_frame_ns <= timeout_ns_;
    if (fresh) {
        if (!healthy_) {
            host.StatusActive("Virtual Desktop 数据已恢复");
            reported_ = false;
        }
        healthy_ = true;
        return true;
    }
    healthy_ = false;
    if (reported_) {
        return false;
    }
    reported_ = true;
    if (last_frame_ns == 0) {
        host.StatusError("Virtual Desktop 还没有给出可用面捕帧：确认 VD Streamer（≥1.30）正在运行，"
                         "串流端设置里已开启面部追踪转发（Forward tracking data to PC），头显侧已启用面捕");
    } else {
        const std::uint64_t silent_seconds = (now_ns - last_frame_ns) / 1000000000ull;
        host.StatusError("Virtual Desktop 面捕数据已 " + std::to_string(silent_seconds) +
                         " 秒没有更新：检查 VD Streamer 是否还在运行、面部追踪转发是否被关闭");
    }
    return false;
}

} // namespace vd
