#include "router.hpp"

#include "loop.hpp"
#include "participant.hpp"
#include "utils.hpp"

#include <rtc/description.hpp>
#include <rtc/rtc.hpp>

#include "external/jwt-cpp/include/jwt-cpp/jwt.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace sfu {

struct Client {
    std::optional<RoomId> roomId;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::string ErrorMessage;

    std::shared_ptr<rtc::Track> track;
};

namespace {

using json = nlohmann::json;

std::optional<std::tuple<uint64_t, uint64_t>> ValidateOffer(const json& offer, std::shared_ptr<Client> client, const std::string& publicKey) {
    if (!offer.contains("token")) {
        client->ErrorMessage = "Offer doesn't contain token";
        return {};
    }

    try {
        auto decoded = jwt::decode(offer.at("token"));

        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::rs256(publicKey, "", "", ""));

        verifier.verify(decoded);

        if (!decoded.has_payload_claim("room")) {
            client->ErrorMessage = "Offer doesn't contain room";
            return {};
        }
        
        if (!decoded.has_payload_claim("user_id")) {
            client->ErrorMessage = "Offer doesn't contain user_id";
            return {};
        }

        return std::make_tuple(decoded.get_payload_claim("user_id").as_integer(), decoded.get_payload_claim("room").as_integer());
    } catch (const jwt::error::token_verification_exception& ex) {
        client->ErrorMessage  = (std::string("Verification failed: ") + ex.what());
    } catch (const std::exception& ex) {
        client->ErrorMessage  = ex.what();
    }
   
    return {};
}

} // namespace

Router::Router()
    : Loop_(std::make_shared<Loop>())
{
    PublicKey_ = ReadPemFile("data/public.pem");
    if (PublicKey_.empty()) {
        std::cerr << "Public key is empty!" << "\n";
        exit(1);
    }
}

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
            if (clientToClose->roomId) {
                Rooms_.at(*clientToClose->roomId).RemoveParticipant(idToClose);
            }

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
            ws->close();
            return;
        }

        auto typeIt = j.find("type");
        if (typeIt == j.end() || !typeIt->is_string()) {
            std::cerr << "[Client " << clientId << "] Signaling message missing type" << std::endl;
            ws->close();
            return;
        }

        const std::string type = *typeIt;
        std::cout << "[Client " << clientId << "] Received signaling: " << type << std::endl;

        if (type == "offer") {
            auto validationResult = ValidateOffer(j, client, PublicKey_);

            if (!j.contains("sdp")) {
                std::cerr << "[Client " << clientId << "] Offer missing sdp" << std::endl;
                ws->close();
                return;
            }
            std::string sdp = j.at("sdp");

            if (!client->pc) {
                rtc::Configuration config;
                config.disableAutoNegotiation = true;
                config.forceMediaTransport = true;
                config.portRangeBegin = 50001;
                config.portRangeEnd = 50005;

                config.iceServers.emplace_back("stun:stun.l.google.com:19302");
                
                std::cout << "[Client " << clientId << "] Creating PeerConnection" << std::endl;
                client->pc = std::make_shared<rtc::PeerConnection>(config);

                client->pc->onLocalDescription([ws, client, clientId](const rtc::Description& desc) {
                    std::cout << "[Client " << clientId << "] Local description type: " << desc.typeString() << std::endl;
                    std::cout << std::string(desc) << "\n";
                    
                    json answer = {
                        {"type", desc.typeString()},
                        {"sdp", std::string(desc)}
                    };
                    if (!client->ErrorMessage.empty()) {
                        answer["error"] = client->ErrorMessage;
                    }

                    std::cout << "[Client " << clientId << "] Sending answer" << std::endl;
                    ws->send(answer.dump());
                    if (!client->ErrorMessage.empty()) {
                        std::cerr << client->ErrorMessage << "\n";
                        ws->close();
                    }
                });

                if (!client->ErrorMessage.empty()) {
                    client->pc->setRemoteDescription(rtc::Description(sdp, "offer"));
                    client->pc->setLocalDescription();
                }

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

                client->pc->onTrack([&, client, clientId](std::shared_ptr<rtc::Track> track) {
                    auto session = std::make_shared<rtc::RtcpReceivingSession>();
                    track->setMediaHandler(session);
                    client->track = track;
                });

                client->pc->onStateChange([this, client, clientId](rtc::PeerConnection::State state) {
                    Loop_->EnqueueTask([this, client, clientId, state] {
                        if (state == rtc::PeerConnection::State::Connected) {
                            if (Rooms_.at(*client->roomId).HasParticipant(clientId)) {
                                return;
                            }

                            std::cout << "[Client " << clientId << "Connected to room: " << *client->roomId << "\n";

                            auto newParticipant = std::make_shared<Participant>(client->pc);
                            Rooms_.at(*client->roomId).AddParticipant(clientId, newParticipant);
                            std::cout << "Handle track for client: " << clientId << "\n";
                            Rooms_.at(*client->roomId).HandleTrackForParticipant(clientId, client->track);
                        }
                    });
                });
            }

            auto [_, roomId] = *validationResult;
            client->roomId = roomId;
            if (!Rooms_.count(*client->roomId)) {
                Rooms_.emplace(*client->roomId, Room{});
            }

            std::cout << "[Client " << clientId << "] Processing offer..." << std::endl;
            
            client->pc->setRemoteDescription(rtc::Description(sdp, "offer"));
            std::cout << "[Client " << clientId << "] Remote description set" << std::endl;
            
            client->pc->setLocalDescription();
            std::cout << "[Client " << clientId << "] Local description set (answer will be generated)" << std::endl;
        }
        else if (type == "answer") {
            auto sdpIt = j.find("sdp");
            if (sdpIt == j.end() || !sdpIt->is_string()) {
                std::cerr << "[Client " << clientId << "] Offer missing sdp" << std::endl;
                return;
            }
            client->pc->setRemoteDescription(rtc::Description(std::string(*sdpIt), "answer"));
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