#include "DrfClient.h"

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

namespace {
constexpr const char* DRF_URL = "wss://drf.rs/ws";
constexpr const char* AUTH_FAILED_CLOSE_REASON = "no valid session provided";

// DRF occasionally fails to snapshot the wallet after a map change and, as
// a fallback, sends the ENTIRE current wallet as if it were a single drop
// instead of just the difference - which would massively inflate every
// currency total. A real drop from actual gameplay never touches more than
// a handful of currencies at once, so anything larger is that known bug,
// not a real drop.
constexpr size_t MAX_CURRENCIES_IN_A_SINGLE_DROP = 10;

void ParseIdDeltaMap(const nlohmann::json& obj, std::unordered_map<int64_t, int64_t>& out)
{
    if (!obj.is_object()) {
        return;
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        try {
            int64_t id = std::stoll(it.key());
            out[id] += it.value().get<int64_t>();
        } catch (const std::exception&) {
            // malformed id/delta - skip this entry rather than dropping the whole message
        }
    }
}
} // namespace

DrfClient::DrfClient()
{
    // ix::initNetSystem()/uninitNetSystem() are process-global (WSAStartup
    // on Windows) and shared with Gw2Api's HttpClient - called once from
    // main.cpp's Load()/Unload() instead of per-class.
    _ws = std::make_unique<ix::WebSocket>();
    _ws->setUrl(DRF_URL);
    _ws->setPingInterval(45);
    _ws->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) { OnMessage(msg); });
}

DrfClient::~DrfClient()
{
    Stop();
}

void DrfClient::SetToken(const std::string& token)
{
    {
        std::lock_guard<std::mutex> lock(_tokenMutex);
        _token = token;
    }

    _ws->stop();

    if (token.empty()) {
        _status = DrfConnectionStatus::Disconnected;
        return;
    }

    _status = DrfConnectionStatus::Connecting;
    _ws->start();
}

void DrfClient::Stop()
{
    _ws->stop();
    _status = DrfConnectionStatus::Disconnected;
}

std::vector<DrfDrop> DrfClient::DrainDrops()
{
    std::lock_guard<std::mutex> lock(_dropsMutex);
    std::vector<DrfDrop> drained = std::move(_pendingDrops);
    _pendingDrops.clear();
    return drained;
}

void DrfClient::OnMessage(const ix::WebSocketMessagePtr& msg)
{
    // This runs on ixwebsocket's own internal thread - an exception escaping
    // it calls std::terminate() and takes down the whole game process, not
    // just this addon, so nothing here may throw uncaught.
    try {
        OnMessageImpl(msg);
    } catch (...) {
        // drop this event; the connection/reconnect logic is unaffected
    }
}

void DrfClient::OnMessageImpl(const ix::WebSocketMessagePtr& msg)
{
    switch (msg->type) {
        case ix::WebSocketMessageType::Open: {
            std::string token;
            {
                std::lock_guard<std::mutex> lock(_tokenMutex);
                token = _token;
            }
            _ws->send("Bearer " + token);
            // DRF sends no auth-success ack - optimistically mark connected,
            // same as the reference C# client does.
            _status = DrfConnectionStatus::Connected;
            break;
        }
        case ix::WebSocketMessageType::Close: {
            _status = (msg->closeInfo.reason == AUTH_FAILED_CLOSE_REASON)
                ? DrfConnectionStatus::AuthFailed
                : DrfConnectionStatus::Connecting; // ixwebsocket auto-reconnects
            break;
        }
        case ix::WebSocketMessageType::Error: {
            _status = DrfConnectionStatus::Connecting; // ixwebsocket auto-reconnects
            break;
        }
        case ix::WebSocketMessageType::Message: {
            if (msg->binary) {
                break; // ignore unexpected binary frames, matches reference client
            }
            try {
                nlohmann::json doc = nlohmann::json::parse(msg->str);
                if (doc.value("kind", "") != "data") {
                    break; // ignore session_update (map/character change) messages
                }

                const auto& drop = doc.at("payload").at("drop");
                if (drop.contains("curr") && drop.at("curr").size() > MAX_CURRENCIES_IN_A_SINGLE_DROP) {
                    break; // whole-wallet-dump bug - see MAX_CURRENCIES_IN_A_SINGLE_DROP's comment
                }

                DrfDrop parsed;
                if (drop.contains("items")) {
                    ParseIdDeltaMap(drop.at("items"), parsed.items);
                }
                if (drop.contains("curr")) {
                    ParseIdDeltaMap(drop.at("curr"), parsed.currencies);
                }

                std::lock_guard<std::mutex> lock(_dropsMutex);
                _pendingDrops.push_back(std::move(parsed));
            } catch (const nlohmann::json::exception&) {
                // malformed message - drop it, keep the connection alive
            }
            break;
        }
        default:
            break;
    }
}
