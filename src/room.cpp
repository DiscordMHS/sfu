#include "room.hpp"

#include "loop.hpp"

#include <rtc/description.hpp>
#include <rtc/rtc.hpp>

namespace sfu {

void NegotiateConnection(const std::shared_ptr<rtc::PeerConnection>& pc) {
    if (pc->state() == rtc::PeerConnection::State::Connected || 
        pc->state() == rtc::PeerConnection::State::New)
    {
        pc->setLocalDescription(rtc::Description::Type::Offer);
    }
}

void Room::AddParticipant(ClientId newClientId, const std::shared_ptr<Participant>& participant) {
    for (auto& [id, other] : Participants_) {
        std::cout << "Adding existing track from participant " << id << " to participant " << newClientId << std::endl;

        auto remoteTrack = participant->GetConnection()->addTrack(other->GetAudioTrack()->description().reciprocate());
        other->AddRemoteTrack(newClientId, remoteTrack);
    }

    Participants_[newClientId] = participant;
    NegotiateConnection(participant->GetConnection());
}

void Room::HandleTrackForParticipant(ClientId clientId, const std::shared_ptr<rtc::Track>& recvTrack) {
    auto& participant = Participants_.at(clientId);

    participant->SetAudioTrack(recvTrack);
    for (auto& [id, other] : Participants_) {
        if (id == clientId) {
            continue;
        }

        std::cout << "Adding track from participant " << clientId << " to participant " << id << std::endl;

        auto remoteTrack = other->GetConnection()->addTrack(recvTrack->description().reciprocate());
        participant->AddRemoteTrack(id, remoteTrack);
        NegotiateConnection(other->GetConnection());
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
