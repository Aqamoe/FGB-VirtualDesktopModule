// 走真实 ABI 的端到端冒烟：加载 DLL → query → initialize → 以 VD 串流端的身份
// 创建同名共享内存 VirtualDesktop.BodyState（先全零，再写合成帧 + SetEvent）→
// update → 检查提交帧。
//
// 名字一手核对自上游 TrackingModule.cs 的字符串字面量（裸名、无 Global\/Local\
// 前缀 → 当前会话 Local 命名空间）；冒烟与模块同会话，所以直接同名即可。
// 它会创建真实命名对象，运行前别和本机正在运行的 VD Streamer 撞车。
// 因此不挂进 ctest，只由 `cmake --build ... --target smoke-test` 触发。

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"
#include "vd_payload.hpp"
#include "fgb_module_api.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr std::size_t kMapBytes = 4096;

// 由模块头里的 FaceState 镜像合成一帧；写侧和读侧共用同一布局定义，
// 偏移本身已由 test_payload 的 static_assert/offsetof 断言锁死。
void FillSyntheticFrame(vd::FaceState& s) {
    std::memset(&s, 0, sizeof(s));
    s.face_flags = vd::kFaceFlagExpressions;
    s.eye_following_valid = 1;
    s.left_eye_valid = 1;
    s.right_eye_valid = 1;
    s.left_eye_pose.orientation.w = 1.0f;   // 单位四元数 = 看向正前方（-Z）
    s.right_eye_pose.orientation.w = 1.0f;
    s.left_eye_confidence = 0.9f;
    s.right_eye_confidence = 0.9f;
    s.weights[vd::kExprJawDrop] = 0.62f;
    s.weights[vd::kExprLipCornerPullerL] = 0.4f;
    s.weights[vd::kExprUpperLidRaiserL] = 0.3f;
    s.weights[vd::kExprEyesClosedL] = 0.25f;
    s.weights[vd::kExprLidTightenerL] = 0.2f;
}

struct FakeStreamer {
    HANDLE mapping = nullptr;
    void* view = nullptr;
    HANDLE event = nullptr;

    bool Create(std::string& error) {
        mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(kMapBytes), vd::kStateMapName);
        if (mapping == nullptr) {
            error = "CreateFileMapping 失败，错误码 " + std::to_string(::GetLastError()) +
                    "（本机是否有 VD Streamer 正在运行抢占同名映射？）";
            return false;
        }
        view = ::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, kMapBytes);
        if (view == nullptr) {
            error = "MapViewOfFile 失败";
            return false;
        }
        event = ::CreateEventW(nullptr, FALSE, FALSE, vd::kStateEventName);
        if (event == nullptr) {
            error = "CreateEvent 失败，错误码 " + std::to_string(::GetLastError());
            return false;
        }
        return true;
    }

    void Write(const vd::FaceState& s) const {
        std::memcpy(view, &s, sizeof(s));
    }

    void Signal() const { ::SetEvent(event); }

    void Close() {
        if (view != nullptr) {
            ::UnmapViewOfFile(view);
        }
        if (event != nullptr) {
            ::CloseHandle(event);
        }
        if (mapping != nullptr) {
            ::CloseHandle(mapping);
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法：%s <virtual-desktop.dll 路径>\n", argv[0]);
        return 2;
    }

    // 先立好「串流端」：全零映射 + 事件，模拟「VD 在跑但没在端面捕」。
    FakeStreamer streamer;
    std::string setup_error;
    if (!streamer.Create(setup_error)) {
        std::printf("冒烟环境准备失败：%s\n", setup_error.c_str());
        return 1;
    }

    HMODULE library = ::LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(library != nullptr);
    if (library == nullptr) {
        std::printf("LoadLibrary 失败，错误码 %lu（DLL 是否已构建？依赖是否在同目录？）\n",
                    ::GetLastError());
        streamer.Close();
        return CheckSummary("virtual-desktop-abi-smoke");
    }

    const auto query = reinterpret_cast<decltype(&fgb_module_query_v2)>(
        ::GetProcAddress(library, "fgb_module_query_v2"));
    CHECK(query != nullptr);

    fgb::kit::testing::HostMock host;
    FgbInitParams params = host.MakeParams();

    const FgbModule* module = query(FGB_MODULE_ABI_V2);
    CHECK(module != nullptr);
    if (module == nullptr) {
        ::FreeLibrary(library);
        streamer.Close();
        return CheckSummary("virtual-desktop-abi-smoke");
    }
    CHECK(module->id == std::string{"com.fgbbridge.virtual-desktop"});
    CHECK_EQ(module->abi_version, FGB_MODULE_ABI_V2);
    CHECK_EQ(module->capabilities,
             static_cast<std::uint32_t>(FGB_CAP_EXPRESSION | FGB_CAP_EYES | FGB_CAP_GAZE));
    CHECK(query(FGB_MODULE_ABI_V2 + 1) == nullptr);  // ABI 不匹配必须拒绝

    void* self = nullptr;
    CHECK_EQ(module->initialize(&self, host.services(), &params), 1);
    if (self == nullptr) {
        std::printf("initialize 失败：%s\n", host.LastStatusDetail().c_str());
        ::FreeLibrary(library);
        streamer.Close();
        return CheckSummary("virtual-desktop-abi-smoke");
    }

    // 内容全零：映射在、事件在，但没有面捕数据 → 不能提交帧。
    for (int i = 0; i < 3; ++i) {
        module->update(self);
    }
    CHECK(host.committed().empty());

    // 串流端写入合成帧并置事件 → 下一次 update 必须提交校验通过的帧。
    vd::FaceState frame{};
    FillSyntheticFrame(frame);
    streamer.Write(frame);
    streamer.Signal();
    for (int attempt = 0; attempt < 200 && host.committed().empty(); ++attempt) {
        module->update(self);
        if (attempt % 8 == 7) {
            streamer.Signal();  // 模拟持续出新帧（内容不变也要有事件信号）
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(!host.committed().empty());

    if (!host.committed().empty()) {
        const FgbFrameSlot& slot = host.committed().back();
        CHECK((slot.attributes & FGB_ATTR_EXPRESSION) != 0);
        CHECK((slot.attributes & FGB_ATTR_LEFT_OPENNESS) != 0);
        CHECK((slot.attributes & FGB_ATTR_LEFT_GAZE) != 0);
        CHECK((slot.attributes & FGB_ATTR_RIGHT_GAZE) != 0);
        CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.62, 1e-5);
        CHECK_NEAR(slot.shapes[host.IndexOf("MouthCornerPullLeft")], 0.4, 1e-5);
        CHECK_NEAR(slot.shapes[host.IndexOf("EyeWideLeft")], 0.3, 1e-5);
        // 睁眼度 = 1 - clamp(0.25 + 0.25 * 0.2)
        CHECK_NEAR(slot.left_eye.openness, 1.0f - (0.25f + 0.25f * 0.2f), 1e-5);
        CHECK_NEAR(slot.right_eye.openness, 1.0f, 1e-6);
        // 单位四元数 → 注视方向 -Z 前，必须归一
        const float len = std::sqrt(slot.left_eye.gaze_dir[0] * slot.left_eye.gaze_dir[0] +
                                    slot.left_eye.gaze_dir[1] * slot.left_eye.gaze_dir[1] +
                                    slot.left_eye.gaze_dir[2] * slot.left_eye.gaze_dir[2]);
        CHECK_NEAR(len, 1.0, 1e-5);
        CHECK_NEAR(slot.left_eye.gaze_dir[2], -1.0, 1e-5);
        CHECK_NEAR(slot.left_eye.gaze_confidence, 0.9, 1e-5);
        CHECK(host.HasStatus(FGB_STATUS_STARTING) || host.HasStatus(FGB_STATUS_ACTIVE));
    }

    module->teardown(self);
    ::FreeLibrary(library);
    streamer.Close();
    return CheckSummary("virtual-desktop-abi-smoke");
}
