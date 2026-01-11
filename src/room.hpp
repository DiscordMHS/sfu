#pragma once

#include "fwd.hpp"
#include "participant.hpp"

#include <rtc/description.hpp>

#include <memory>
#include <unordered_map>
#include <span>

namespace sfu {

class Loop;

using ClientId = uint64_t;

class Room {
public:
    Room() = default;

    void AddParticipant(ClientId clientId, const std::shared_ptr<Participant>& participant);
    void RemoveParticipant(ClientId clientId);

    bool HasParticipant(ClientId clientId) {
        return Participants_.count(clientId);
    }

    const auto& GetParticipants() {
        return Participants_;
    } 

    void HandleTracksForParticipant(ClientId clientId, const std::array<std::shared_ptr<rtc::Track>, 2> tracks);

private:
    uint64_t GetUniqueId() {
        return UniqueIdGenerator_++;
    }

    std::atomic<uint64_t> UniqueIdGenerator_ = 150;
    std::unordered_map<ClientId, std::shared_ptr<Participant>> Participants_;
};

} // namespace sfu
