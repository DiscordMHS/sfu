#pragma once

#include "fwd.hpp"
#include "room.hpp"

#include <memory>
#include <unordered_map>

namespace sfu {

using RoomId = uint64_t;

class Loop;
class Client;

class Router {
public:
    Router();
    void Run();

private:
    void WsOpenCallback(std::shared_ptr<rtc::WebSocket> ws);
    void WsClosedCallback(std::shared_ptr<rtc::WebSocket> ws);
    void WsOnMessageCallback(std::shared_ptr<rtc::WebSocket> ws, rtc::message_variant&& message);

private:
    std::string PublicKey_;
    std::atomic_uint64_t IdGenerator_{1};
    std::set<std::shared_ptr<Client>> Clients_;

    std::map<RoomId, Room> Rooms_;
    std::shared_ptr<Loop> Loop_;
};

} // namespace sfu
