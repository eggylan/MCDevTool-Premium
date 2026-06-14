#include "mcdevtool/safaia/protocol.h"

namespace MCDevTool::Safaia {

    namespace {
        // 小端读写辅助。线缆使用 int32 LE（与 Python struct.pack("ii") 在 x86 上一致）。
        int32_t readI32LE(const uint8_t* p) {
            return static_cast<int32_t>(
                static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
                | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24)
            );
        }

        void appendI32LE(std::vector<uint8_t>& out, int32_t v) {
            auto u = static_cast<uint32_t>(v);
            out.push_back(static_cast<uint8_t>(u & 0xFF));
            out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
        }

        std::string jsonDumpNoThrow(const nlohmann::json& value) {
            try {
                // ensure_ascii=false 保留 UTF-8 中文，error_handler=replace 防止编码异常抛出。
                return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            } catch (...) {
                return "{}";
            }
        }
    } // namespace

    const char* handlerName(int handle) {
        switch (handle) {
            case RPCHandles::NotEnabled:              return "NotEnabled";
            case RPCHandles::NoScreenFound:           return "NoScreenFound";
            case RPCHandles::GetEnabled:              return "GetEnabled";
            case RPCHandles::GameNotInited:           return "GameNotInited";
            case RPCHandles::ScreenChanged:           return "ScreenChanged";
            case RPCHandles::GetControlTree:          return "GetControlTree";
            case RPCHandles::GetControlsData:         return "GetControlsData";
            case RPCHandles::ControlSelectionChanged: return "ControlSelectionChanged";
            default:                                  return "unknown";
        }
    }

    std::vector<Frame> ProtocolFramer::input(const uint8_t* data, size_t len) {
        std::vector<Frame> frames;
        if (data && len > 0) {
            buffer_.append(reinterpret_cast<const char*>(data), len);
        }

        size_t offset = 0;
        const size_t total = buffer_.size();
        while (total - offset >= 8) {
            const uint8_t* head   = reinterpret_cast<const uint8_t*>(buffer_.data()) + offset;
            int32_t        protoId = readI32LE(head);
            int32_t        rawLen  = readI32LE(head + 4);

            // 防御异常长度：负数或超过上限直接丢弃缓冲，避免无限增长 / 越界。
            if (rawLen < 0 || static_cast<size_t>(rawLen) > kMaxPacketLength) {
                buffer_.clear();
                return frames;
            }
            size_t dataLen = static_cast<size_t>(rawLen);
            if (total - offset < dataLen + 8) {
                break; // 帧未完整，等待更多数据
            }

            Frame f;
            f.protocolId = protoId;
            f.payload.assign(buffer_.data() + offset + 8, dataLen);
            frames.push_back(std::move(f));
            offset += dataLen + 8;
        }

        if (offset > 0) {
            buffer_.erase(0, offset);
        }
        return frames;
    }

    std::vector<uint8_t> ProtocolFramer::pack(int32_t protocolId, std::string_view payload) {
        std::vector<uint8_t> out;
        out.reserve(8 + payload.size());
        appendI32LE(out, protocolId);
        appendI32LE(out, static_cast<int32_t>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    std::vector<uint8_t> ProtocolFramer::packJson(int32_t protocolId, const nlohmann::json& payload) {
        return pack(protocolId, jsonDumpNoThrow(payload));
    }

    nlohmann::json buildUidebugerInner(
        const std::string& func, const nlohmann::json& args, const nlohmann::json& kwargs
    ) {
        return nlohmann::json{
            {  "func",                                       func},
            {  "args",     args.is_array() ? args : nlohmann::json::array()},
            {"kwargs", kwargs.is_object() ? kwargs : nlohmann::json::object()},
        };
    }

    nlohmann::json buildUidebugerFrame(const nlohmann::json& rpcInner) {
        return nlohmann::json{
            {"cmd", "uidebuger_call"},
            {"rpc",          rpcInner},
        };
    }

    std::optional<nlohmann::json> extractUidebugerResult(const nlohmann::json& outer) {
        if (!outer.is_object()) {
            return std::nullopt;
        }
        if (outer.value("cmd", std::string{}) != "uidebuger_call") {
            return std::nullopt;
        }
        if (!outer.contains("result")) {
            return std::nullopt;
        }
        const auto& result = outer["result"];
        // result 可能是 json 字符串（真机形态），也可能已是对象（容错）。
        if (result.is_string()) {
            auto inner = nlohmann::json::parse(result.get<std::string>(), nullptr, false);
            if (inner.is_discarded()) {
                return std::nullopt;
            }
            return inner;
        }
        if (result.is_object()) {
            return result;
        }
        return std::nullopt;
    }

    std::optional<nlohmann::json> parseUidebugerResponse(const std::string& rawPayload) {
        auto outer = nlohmann::json::parse(rawPayload, nullptr, false);
        if (outer.is_discarded()) {
            return std::nullopt;
        }
        return extractUidebugerResult(outer);
    }

    std::string buildDiscoveryPayload(const std::string& serverIp, int serverPort) {
        nlohmann::json j{
            {  "ip", serverIp},
            {"port", serverPort},
        };
        return jsonDumpNoThrow(j);
    }

} // namespace MCDevTool::Safaia
