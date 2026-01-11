#include "router.hpp"

#include "loop.hpp"
#include "participant.hpp"
#include "rtc/rtpdepacketizer.hpp"
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
    std::optional<ClientId> ClientId;
    std::optional<RoomId> RoomId;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::string ErrorMessage;
    bool IsVideoActive = false;

    std::array<std::shared_ptr<rtc::Track>, 2> Tracks;
};

namespace {

constexpr std::string_view AUDIO = "0";
constexpr std::string_view VIDEO = "1";

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

        auto roomId = decoded.get_payload_claim("room").as_integer();
        auto clientId = decoded.get_payload_claim("user_id").as_integer();
        if (roomId <= 0 || clientId <= 0) {
            client->ErrorMessage = "Invalid room or user id";
            return {};
        }

        return std::make_tuple(clientId, roomId);
    } catch (const jwt::error::token_verification_exception& ex) {
        client->ErrorMessage = (std::string("Verification failed: ") + ex.what());
    } catch (const std::exception& ex) {
        client->ErrorMessage = ex.what();
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
        auto client = std::make_shared<Client>();
        Clients_.emplace(client);
        client->ws = ws;
    });
}

void Router::WsClosedCallback(std::shared_ptr<rtc::WebSocket> ws) {
    Loop_->EnqueueTask([this, ws = std::move(ws)]
    {
        std::shared_ptr<Client> clientToClose;
        {
            for (auto it = Clients_.begin(); it != Clients_.end(); ++it) {
                if ((*it)->ws == ws) {
                    clientToClose = *it;
                    break;
                }
            }
        }

        if (clientToClose) {
            if (clientToClose->RoomId) {
                std::cout << "[Client " << *clientToClose->ClientId << "] WebSocket disconnected" << std::endl;
                Rooms_.at(*clientToClose->RoomId).RemoveParticipant(*clientToClose->ClientId);
            }

            if (clientToClose->pc) {
                clientToClose->pc->close();
            }
            Clients_.erase(clientToClose);
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
        {
            for (auto& c : Clients_) {
                if (c->ws == ws) {
                    client = c;
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
            std::cerr << "Signaling message missing type" << std::endl;
            ws->close();
            return;
        }

        const auto& type = *typeIt;

        if (type != "offer" && (!client->ClientId || !client->RoomId)) {
            std::cerr << "Invalid message type" << std::endl;
            ws->close();
            return;
        }

        if (type == "offer") {
            auto validationResult = ValidateOffer(j, client, PublicKey_);

            if (!validationResult) {
                ws->send(client->ErrorMessage);
                ws->close();
                return;
            }

            auto [clientId, roomId] = *validationResult;

            if (Rooms_.contains(roomId) && Rooms_[roomId].HasParticipant(clientId)) {
                Rooms_[roomId].RemoveParticipant(clientId);
                for (auto it = Clients_.begin(); it != Clients_.end(); ++it) {
                    if ((*it)->ClientId == clientId) {
                        client->ws->close();
                        break;
                    }
                }
            }

            client->ClientId = clientId;
            client->RoomId = roomId; 

            if (!j.contains("sdp")) {
                std::cerr << "Offer missing sdp" << std::endl;
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

                    json answer = {
                        {"type", desc.typeString()},
                        {"sdp", std::string(desc)}
                    };

                    std::cout << "[Client " << clientId << "] Sending answer" << std::endl;
                    ws->send(answer.dump());
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
                    Loop_->EnqueueTask([this, client, clientId, track] {
                        auto session = std::make_shared<rtc::RtcpReceivingSession>();
                        track->setMediaHandler(session);

                        std::cout << track->mid() << "\n";
                        if (track->mid() == AUDIO) {
                            client->Tracks[0] = track;
                            return;
                        }

                        client->Tracks[1] = track;
                    });
                });

                client->pc->onStateChange([this, client](rtc::PeerConnection::State state) {
                    Loop_->EnqueueTask([this, client, state] {
                        if (state == rtc::PeerConnection::State::Connected) {
                            std::cout << "[Client " << *client->ClientId << "Connected to room: " << *client->RoomId << "\n";

                            auto newParticipant = std::make_shared<Participant>(client->pc, *client->ClientId);
                            Rooms_[*client->RoomId].AddParticipant(*client->ClientId, newParticipant);
                            std::cout << "Handle tracks for client: " << *client->ClientId << "\n";
                            Rooms_[*client->RoomId].HandleTracksForParticipant(*client->ClientId, client->Tracks);
                        }
                    });
                });
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
                std::cerr << "[Client " << *client->ClientId << "] Offer missing sdp" << std::endl;
                return;
            }
            client->pc->setRemoteDescription(rtc::Description(std::string(*sdpIt), "answer"));
        }
        else if (type == "candidate") {
            auto candIt = j.find("candidate");
            if (candIt == j.end() || !candIt->is_string()) {
                std::cerr << "[Client " << *client->ClientId << "] Candidate message missing candidate field" << std::endl;
                return;
            }

            std::string candidate = *candIt;
            
            // Skip empty candidates
            if (candidate.empty()) {
                std::cout << "[Client " << *client->ClientId << "] Skipping empty candidate" << std::endl;
                return;
            }

            std::string sdpMid = j.value("sdpMid", "");
            
            std::cout << "[Client " << *client->ClientId << "] Adding remote candidate: " << candidate << std::endl;

            if (client->pc) {
                try {
                    client->pc->addRemoteCandidate(rtc::Candidate(candidate, sdpMid));
                    std::cout << "[Client " << *client->ClientId << "] ✓ Candidate added successfully" << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "[Client " << *client->ClientId << "] Failed to add candidate: " << e.what() << std::endl;
                }
            }
        }
        else if (type == "mode") {
            auto& room = Rooms_[*client->RoomId];
            bool isActive = j["active"].get<bool>();

            client->IsVideoActive = isActive;

            const auto& participants = room.GetParticipants();
            const auto& outgoingTracks = participants.at(*client->ClientId)->GetOutgoingTracks();
            for (auto& other : Clients_) {
                if (other == client) {
                    continue;
                }

                if (auto it = outgoingTracks.find(*other->ClientId); it != outgoingTracks.cend()) {
                    other->ws->send(
                        json{
                            {"type", "mode"},
                            {"ssrc", outgoingTracks.at(*other->ClientId)[1]->description().getSSRCs()[0] },
                            {"active", isActive}
                        }.dump());
                }
            }
        }
        else if (type == "endOfCandidates") {
            std::cout << "[Client " << *client->ClientId << "] Client finished sending candidates" << std::endl;
        }
        else if (type == "ping") {
            ws->send(json({{"type","pong"}}).dump());
        }
        else {
            std::cout << "[Client " << *client->ClientId << "] Unknown message type: " << type << std::endl;
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