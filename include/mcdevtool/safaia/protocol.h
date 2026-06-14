#pragma once
// Safaia 协议常量与编解码。
// 线缆 framing: [int32 LE protocol_id][int32 LE len][payload(UTF-8)]。
// 本模块为纯逻辑，无 Winsock 依赖，可独立单测。
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace MCDevTool::Safaia {

    // MCProtocol 命令 ID（线缆帧的 protocol_id 字段）。
    namespace MCProtocol {
        inline constexpr int32_t connect         = 1;
        inline constexpr int32_t heart           = 2;  // 心跳（游戏 3s 超时）
        inline constexpr int32_t config          = 3;  // 握手配置
        inline constexpr int32_t message         = 4;  // 日志消息
        inline constexpr int32_t directory       = 5;
        inline constexpr int32_t ftp             = 6;
        inline constexpr int32_t cmd             = 7;  // UI debugger 主通道
        inline constexpr int32_t file_transfer   = 16;
        inline constexpr int32_t run_client      = 22;
        inline constexpr int32_t run_server      = 23;
        inline constexpr int32_t leave           = 32;
        inline constexpr int32_t connect_success = 48;
        inline constexpr int32_t connect_block   = 49;
    } // namespace MCProtocol

    // UI debugger RPC 响应 handle ID（内层 result.handle 字段）。
    namespace RPCHandles {
        inline constexpr int NotEnabled              = 0;
        inline constexpr int NoScreenFound           = 1;
        inline constexpr int GetEnabled              = 2;
        inline constexpr int GameNotInited           = 3;
        inline constexpr int ScreenChanged           = 4; // 带外事件：切屏
        inline constexpr int GetControlTree          = 5;
        inline constexpr int GetControlsData         = 6;
        inline constexpr int ControlSelectionChanged = 7; // 带外事件：选中变化
    } // namespace RPCHandles

    // 返回 handle 的可读名（未知返回 "unknown"）。
    const char* handlerName(int handle);

    // 该 handle 是否为带外事件（不占用在途请求，单独分发）。
    inline bool isOutOfBandHandle(int handle) {
        return handle == RPCHandles::ScreenChanged || handle == RPCHandles::ControlSelectionChanged;
    }

    // 设备发现：游戏侧监听的 UDP 端口范围 26613..26622。
    namespace ConstPort {
        inline constexpr int client_listen_base  = 26613;
        inline constexpr int client_listen_count = 10; // 26613..26622
    } // namespace ConstPort

    // 单帧解析结果。
    struct Frame {
        int32_t     protocolId = 0;
        std::string payload; // 原始 payload 字节（通常为 UTF-8 JSON 文本）
    };

    // 流式 framer：按 [int32 LE id][int32 LE len][payload] 切分。
    // 等价于 Safaia 的 SimpleProtocolFilter。非线程安全，由单一读线程使用。
    class ProtocolFramer {
    public:
        // 喂入原始字节，返回本次能完整解出的所有帧；不完整尾部留在内部缓冲。
        std::vector<Frame> input(const uint8_t* data, size_t len);

        size_t buffered() const { return buffer_.size(); }
        void   reset() { buffer_.clear(); }

        // 打包单帧（payload 已是字节）。
        static std::vector<uint8_t> pack(int32_t protocolId, std::string_view payload);
        // 打包单帧（payload 为 JSON，紧凑序列化为 UTF-8）。命名区分以避免 std::string 重载歧义。
        static std::vector<uint8_t> packJson(int32_t protocolId, const nlohmann::json& payload);

        // 单帧 payload 上限（防御异常长度），与 IPC 层保持一致。
        static constexpr size_t kMaxPacketLength = 16 * 1024 * 1024;

    private:
        std::string buffer_;
    };

    // ── uidebuger_call 信封编解码 ───────────────────────────────────

    // 内层 RPC payload：{"func":..,"args":[..],"kwargs":{..}}。
    nlohmann::json buildUidebugerInner(
        const std::string&    func,
        const nlohmann::json& args   = nlohmann::json::array(),
        const nlohmann::json& kwargs = nlohmann::json::object()
    );

    // 外层 cmd=7 payload：{"cmd":"uidebuger_call","rpc":{..}}。
    nlohmann::json buildUidebugerFrame(const nlohmann::json& rpcInner);

    // 从已解析的外层 JSON（{"cmd":"uidebuger_call","result":"<json 字符串>"}）
    // 提取内层结果 {handle,success,data}。非 uidebuger_call 或解析失败返回 nullopt。
    std::optional<nlohmann::json> extractUidebugerResult(const nlohmann::json& outer);

    // 从 cmd=7 帧的原始 payload 字符串解析内层结果。失败返回 nullopt。
    std::optional<nlohmann::json> parseUidebugerResponse(const std::string& rawPayload);

    // 设备发现 UDP payload：{"ip":server_ip,"port":server_port}（UTF-8）。
    std::string buildDiscoveryPayload(const std::string& serverIp, int serverPort);

} // namespace MCDevTool::Safaia
