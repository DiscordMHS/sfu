#include "room.hpp"

#include "loop.hpp"

#include <rtc/description.hpp>
#include <rtc/rtc.hpp>
#include <stdexcept>

namespace sfu {

void Room::AddParticipant(ClientId newClientId, const std::shared_ptr<Participant>& participant) {
    bool needRenegotiate = false;

    for (auto& [id, other] : Participants_) {
        std::cout << "Adding existing track from participant " << id << " to participant " << newClientId << std::endl;

        rtc::Description::Audio descr("audio", rtc::Description::Direction::SendOnly);
        descr.addOpusCodec(109);
        auto remoteTrack = participant->GetConnection()->addTrack(descr);

        other->AddRemoteTrack(newClientId, remoteTrack);
        needRenegotiate = true;
    }
    if (needRenegotiate) {
        participant->GetConnection()->setLocalDescription(rtc::Description::Type::Offer);
    }

    Participants_[newClientId] = participant;
}

void Room::HandleTrackForParticipant(ClientId clientId, const std::shared_ptr<rtc::Track>& track) {
    auto& participant = Participants_.at(clientId);

    participant->SetAudioTrack(track);
    for (auto& [id, other] : Participants_) {
        if (id == clientId) {
            continue;
        }

        std::cout << "Adding track from participant " << clientId << " to participant " << id << std::endl;

        rtc::Description::Audio descr("audio", rtc::Description::Direction::SendOnly);
        descr.addOpusCodec(109);
        auto remoteTrack = other->GetConnection()->addTrack(descr);
        participant->AddRemoteTrack(id, remoteTrack);

        other->GetConnection()->setLocalDescription(rtc::Description::Type::Offer);
    }
}

void Room::RemoveParticipant(ClientId clientId) {
    if (!Participants_.count(clientId)) {
        return;
    }

    std::cout << "Removing participant " << clientId << " from the room" << std::endl;

    for (auto& [id, participant] : Participants_) {
        if (id == clientId) {
            continue;
        }

        std::cout << "Removing track from " << id << " to " << clientId << std::endl;

        participant->RemoveRemoteTrack(clientId);
    }

    Participants_.at(clientId)->GetAudioTrack()->close();
    Participants_.erase(clientId);
}

} // namespace sfu
