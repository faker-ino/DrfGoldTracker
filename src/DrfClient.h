// DrfClient - connects to the DRF (drf.rs) live drop feed over websocket.
//
// Protocol (reverse-engineered, since DRF has no public docs - see README
// for credits):
//   - connect to wss://drf.rs/ws
//   - immediately send a single text frame "Bearer <token>" - this IS the
//     auth handshake, there's no JSON envelope for it and no ack message.
//   - server pushes JSON text frames:
//       {"kind":"session_update","payload":{...}}   - map/character changes, ignored here
//       {"kind":"data","payload":{"character":"X","drop":{
//           "items":{"<itemId>":<delta>,...},
//           "curr":{"<currencyId>":<delta>,...},
//           "mf":<magicFind>,"timestamp":"..."}}}
//   - on bad token the server closes the connection with reason
//     "no valid session provided" instead of ever sending a message.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ix { class WebSocket; struct WebSocketMessage; using WebSocketMessagePtr = std::unique_ptr<WebSocketMessage>; }

enum class DrfConnectionStatus {
    Disconnected, // no/empty token - not attempting to connect
    Connecting,
    Connected,
    AuthFailed,
};

struct DrfDrop {
    std::unordered_map<int64_t, int64_t> items;      // itemId -> delta (can be negative)
    std::unordered_map<int64_t, int64_t> currencies;  // currencyId -> delta
};

class DrfClient {
public:
    DrfClient();
    ~DrfClient();

    DrfClient(const DrfClient&) = delete;
    DrfClient& operator=(const DrfClient&) = delete;

    // Starts (or restarts, if already running) the connection using the
    // given DRF token. Passing an empty token stops the connection.
    void SetToken(const std::string& token);

    // Permanently stops the background connection. Call on addon unload.
    void Stop();

    DrfConnectionStatus GetStatus() const { return _status.load(); }

    // Returns and clears all drops received since the last call. Safe to
    // call from the render thread.
    std::vector<DrfDrop> DrainDrops();

private:
    void OnMessage(const ix::WebSocketMessagePtr& msg);
    void OnMessageImpl(const ix::WebSocketMessagePtr& msg);

    std::unique_ptr<ix::WebSocket> _ws;
    std::atomic<DrfConnectionStatus> _status{DrfConnectionStatus::Disconnected};

    std::mutex _tokenMutex;
    std::string _token;

    std::mutex _dropsMutex;
    std::vector<DrfDrop> _pendingDrops;
};
