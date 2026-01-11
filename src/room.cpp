#include "room.hpp"

#include "loop.hpp"
#include "participant.hpp"
#include "rtc/peerconnection.hpp"

#include <rtc/description.hpp>
#include <rtc/rtc.hpp>
#include <stdexcept>
#include <thread>

namespace sfu {

void Room::AddParticipant(ClientId newClientId, const std::shared_ptr<Participant>& participant) {
    Participants_[newClientId] = participant;

    for (auto [id, other] : Participants_) {
        if (id == newClientId) {
            continue;
        }

        std::cout << "Adding existing track from participant " << id << " to participant " << newClientId << std::endl;

        rtc::Description::Audio audioDescr(std::to_string(GetUniqueId()), rtc::Description::Direction::SendOnly);
        audioDescr.addSSRC(GetUniqueId(), "audio", std::to_string(GetUniqueId()) + "audio");
        audioDescr.addOpusCodec(109);
        auto remoteAudioTrack = participant->GetConnection()->addTrack(audioDescr);
    
        rtc::Description::Video videoDescr(std::to_string(GetUniqueId()), rtc::Description::Direction::SendOnly);
        videoDescr.addSSRC(GetUniqueId(), "video", std::to_string(GetUniqueId()) + "video");
        videoDescr.addVP8Codec(120);
        videoDescr.setBitrate(3000);
        auto remoteVideoTrack = participant->GetConnection()->addTrack(videoDescr);

        other->AddRemoteTracks(newClientId, {remoteAudioTrack, remoteVideoTrack});
    }

    participant->GetConnection()->setLocalDescription(rtc::Description::Type::Offer);
}

void Room::HandleTracksForParticipant(ClientId clientId, const std::array<std::shared_ptr<rtc::Track>, 2> tracks) {
    auto& participant = Participants_.at(clientId);

    participant->SetTracks(tracks);
    for (auto [id, other] : Participants_) {
        if (id == clientId) {
            continue;
        }

        std::cout << "Adding tracks from participant " << clientId << " to participant " << id << std::endl;

        rtc::Description::Audio audioDescr(std::to_string(GetUniqueId()), rtc::Description::Direction::SendOnly);
        audioDescr.addSSRC(GetUniqueId(), "audio", std::to_string(GetUniqueId()) + "audio");
        audioDescr.addOpusCodec(109);
        auto remoteAudioTrack = other->GetConnection()->addTrack(audioDescr);
    
        rtc::Description::Video videoDescr(std::to_string(GetUniqueId()), rtc::Description::Direction::SendOnly);
        videoDescr.addSSRC(GetUniqueId(), "video", std::to_string(GetUniqueId()) + "video");
        videoDescr.addVP8Codec(120);
        videoDescr.setBitrate(3000);
        auto remoteVideoTrack = other->GetConnection()->addTrack(videoDescr);

        other->GetConnection()->setLocalDescription(rtc::Description::Type::Offer);

        participant->AddRemoteTracks(id, {remoteAudioTrack, remoteVideoTrack});
    }

    for (auto [id, participant] : Participants_) {
        participant->GetTracks()[1]->requestKeyframe();
    }
}

void Room::RemoveParticipant(ClientId clientId) {
    if (!Participants_.count(clientId)) {
        return;
    }

    std::cout << "Removing participant " << clientId << " from the room" << std::endl;
    Participants_[clientId]->CloseRemoteTracks();

    for (auto& [id, other] : Participants_) {
        if (id == clientId) {
            continue;
        }

        std::cout << "Removing track from " << id << " to " << clientId << std::endl;

        other->RemoveRemoteTracks(clientId);
        other->GetConnection()->setLocalDescription(rtc::Description::Type::Offer);
    }

    for (auto& track : Participants_.at(clientId)->GetTracks()) {
        track->close();
    }
    Participants_.erase(clientId);
}

} // namespace sfu
