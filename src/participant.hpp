#pragma once

#include "fwd.hpp"
#include "rtc/peerconnection.hpp"

#include <rtc/description.hpp>
#include <rtc/rtc.hpp>

#include <memory>
#include <unordered_map>

namespace sfu {

class Loop;

using ClientId = uint64_t;

class Participant {
public:
    Participant(const std::shared_ptr<rtc::PeerConnection>& peerConnection);

    void SetAudioTrack(const std::shared_ptr<rtc::Track>& track);

    void AddRemoteTrack(ClientId clientId, const std::shared_ptr<rtc::Track>& track) {
        std::lock_guard guard(TracksMutex_);
        OutgoingTracks_[clientId] = track;
    }

    void RemoveRemoteTrack(ClientId clientId) {
        std::lock_guard guard(TracksMutex_);
        OutgoingTracks_.at(clientId)->close();
        OutgoingTracks_.erase(clientId);
    }

    std::shared_ptr<rtc::Track> GetAudioTrack() {
        return Track_;
    }

    std::shared_ptr<rtc::PeerConnection> GetConnection() {
        return PeerConnection_;
    }

    std::unordered_map<ClientId, std::shared_ptr<rtc::Track>> OutgoingTracks_;

private:
    std::shared_ptr<rtc::Track> Track_;
    std::shared_ptr<rtc::PeerConnection> PeerConnection_;

    std::mutex TracksMutex_;
};

} // namespace sfu
