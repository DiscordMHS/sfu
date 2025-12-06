#include "router.hpp"

#include "loop.hpp"
#include "participant.hpp"

#include <rtc/description.hpp>
#include <rtc/rtc.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <thread>

namespace sfu {

namespace {

using json = nlohmann::json;

} // namespace

struct Client {
    RoomId roomId;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
};

Router::Router()
    : Loop_(std::make_shared<Loop>())
{ }

void Router::WsOpenCallback(std::shared_ptr<rtc::WebSocket> ws) {
    Loop_->EnqueueTask([this, ws = std::move(ws)]
    {
        auto id = IdGenerator_++;
        auto client = std::make_shared<Client>();
        Clients_.emplace(id, client);
        client->ws = ws;

        std::cout << "[Client " << id << "] WebSocket connected" << std::endl;
    });
}

void Router::WsClosedCallback(std::shared_ptr<rtc::WebSocket> ws) {
    Loop_->EnqueueTask([this, ws = std::move(ws)]
    {
        std::shared_ptr<Client> clientToClose;
        uint32_t idToClose;
        {
            for (auto it = Clients_.begin(); it != Clients_.end(); ++it) {
                if (it->second->ws == ws) {
                    clientToClose = it->second;
                    idToClose = it->first;
                    Clients_.erase(it);
                    break;
                }
            }
        }

        if (clientToClose) {
            std::cout << "[Client " << idToClose << "] WebSocket disconnected" << std::endl;
            Rooms_.at(clientToClose->roomId).RemoveParticipant(idToClose);

            if (clientToClose->pc) {
                clientToClose->pc->close();
            }
        }
    });
}

void Router::WsOnMessageCallback(std::shared_ptr<rtc::WebSocket> ws, rtc::message_variant&& message) {
    Loop_->EnqueueTask([this, ws = std::move(ws), message = std::move(message)]
    {
        auto pstr = std::get_if<std::string>(&message);
        if (!pstr) return;

        json j;
        try {
            j = json::parse(*pstr);
        } catch (...) {
            std::cerr << "Invalid JSON signaling message" << std::endl;
            return;
        }

        std::shared_ptr<Client> client;
        uint64_t clientId;
        {
            for (auto& [id, c] : Clients_) {
                if (c->ws == ws) {
                    client = c;
                    clientId = id;
                    break;
                }
            }
        }
        if (!client) {
            std::cerr << "Client not found for signaling message" << std::endl;
            return;
        }

        auto typeIt = j.find("type");
        if (typeIt == j.end() || !typeIt->is_string()) {
            std::cerr << "[Client " << clientId << "] Signaling message missing type" << std::endl;
            return;
        }

        const std::string type = *typeIt;
        std::cout << "[Client " << clientId << "] Received signaling: " << type << std::endl;

        if (type == "join") {
            auto roomIdIt = j.find("room_id");
            if (roomIdIt == j.end() || !roomIdIt->is_string()) {
                std::cerr << "[Client " << clientId << "] Offer missing room id" << std::endl;
                return;
            }
            
            // Update client state
            client->roomId = std::stoll(std::string{*roomIdIt});

            if (!Rooms_.count(client->roomId)) {
                Rooms_.emplace(client->roomId, Room{});
            }

            if (!client->pc) {
                rtc::Configuration config;
                config.disableAutoNegotiation = true;
                //config.forceMediaTransport = true;

                config.iceServers.emplace_back("stun:stun.l.google.com:19302");
                
                std::cout << "[Client " << clientId << "] Creating PeerConnection" << std::endl;
                client->pc = std::make_shared<rtc::PeerConnection>(config);

                std::cout << "Added new participant " << clientId << " to a room " << client->roomId << "\n";

                client->pc->onLocalDescription([ws, clientId](const rtc::Description& desc) {
                    std::cout << "[Client " << clientId << "] Local description type: " << desc.typeString() << std::endl;
                    std::cout << std::string{desc} << std::endl;

                    json answer = {
                        {"type", desc.typeString()},
                        {"sdp", std::string{desc}}
                    };
                    std::cout << "[Client " << clientId << "] Sending local description" << std::endl;
                    ws->send(answer.dump());
                });

                client->pc->onLocalCandidate([ws, clientId](const rtc::Candidate& cand) {
                    auto candidate = cand.candidate();
                    bool isIPv6 = candidate.find('.') == std::string::npos;
                    if (cand.candidate().empty() || isIPv6) {
                        std::cerr << "Skipping invalid candidate " << cand << std::endl;
                        return;
                    }

                    std::string candStr = cand.candidate();

                    std::cout << "[Client " << clientId << "] Local candidate: " << candStr << std::endl;

                    json jcand = {
                        {"type", "candidate"},
                        {"candidate", candStr}
                    };
                    
                    if (auto mid = cand.mid(); !mid.empty()) {
                        jcand["sdpMid"] = mid;
                    }

                    ws->send(jcand.dump());
                });

                rtc::Description::Audio recv("audio", rtc::Description::Direction::RecvOnly);
                auto recvTrack = client->pc->addTrack(recv);
                recvTrack->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

                client->pc->onStateChange([this, client, clientId, recvTrack](rtc::PeerConnection::State state) {
                    Loop_->EnqueueTask([this, client, clientId, state, recvTrack] {
                        if (state == rtc::PeerConnection::State::Connected) {
                            if (Rooms_.at(client->roomId).HasParticipant(clientId)) {
                                return;
                            }

                            auto newParticipant = std::make_shared<Participant>(client->pc);
                            Rooms_.at(client->roomId).AddParticipant(clientId, newParticipant);

                            std::cout << "Handle track for client: " << clientId << "\n";
                            Rooms_.at(client->roomId).HandleTrackForParticipant(clientId, recvTrack);
                        }
                    });
                });
            }
            client->pc->setLocalDescription(rtc::Description::Type::Offer);

            // std::cout << "[Client " << clientId << "] Remote description set" << std::endl;
        }
        else if (type == "answer") {
            std::cout << "[Client " << clientId << "] Handling answer" << std::endl;
            auto sdpIt = j.find("sdp");
            if (sdpIt == j.end() || !sdpIt->is_string()) {
                std::cerr << "[Client " << clientId << "] answer missing sdp" << std::endl;
                return;
            }

            std::string sdp = *sdpIt;
            client->pc->setRemoteDescription(rtc::Description(sdp, type));
        }
        else if (type == "candidate") {
            auto candIt = j.find("candidate");
            if (candIt == j.end() || !candIt->is_string()) {
                std::cerr << "[Client " << clientId << "] Candidate message missing candidate field" << std::endl;
                return;
            }

            std::string candidate = *candIt;
            
            // Skip empty candidates
            if (candidate.empty()) {
                std::cout << "[Client " << clientId << "] Skipping empty candidate" << std::endl;
                return;
            }

            std::string sdpMid = j.value("sdpMid", "");
            
            std::cout << "[Client " << clientId << "] Adding remote candidate: " << candidate << std::endl;

            if (client->pc) {
                try {
                    client->pc->addRemoteCandidate(rtc::Candidate(candidate, sdpMid));
                    std::cout << "[Client " << clientId << "] ✓ Candidate added successfully" << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "[Client " << clientId << "] Failed to add candidate: " << e.what() << std::endl;
                }
            }
        }
        else if (type == "endOfCandidates") {
            std::cout << "[Client " << clientId << "] Client finished sending candidates" << std::endl;
            // Don't need to add empty candidate - libdatachannel handles this automatically
        }
        else if (type == "ping") {
            ws->send(json({{"type","pong"}}).dump());
        }
        else {
            std::cout << "[Client " << clientId << "] Unknown message type: " << type << std::endl;
        }
    });
}

void Router::Run() {
    rtc::WebSocketServer::Configuration wsCfg;
    wsCfg.port = 8000;
    wsCfg.enableTls = false;

    std::thread t{std::bind(&Loop::Run, Loop_)};

    auto wsServer = std::make_shared<rtc::WebSocketServer>(wsCfg);
    wsServer->onClient([&](std::shared_ptr<rtc::WebSocket> ws) {
        ws->onOpen([&, ws]() mutable {
            WsOpenCallback(ws);
        });

        ws->onClosed([&, ws]() mutable {
            WsClosedCallback(ws);
        });

        ws->onMessage([&, ws](rtc::message_variant message) mutable {
            WsOnMessageCallback(ws, std::move(message));
        });
    });
    t.join();
}

} // namespace sfu