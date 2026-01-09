#pragma once

#include "fwd.hpp"
#include "rtc/peerconnection.hpp"
#include "loop.hpp"

#include <rtc/description.hpp>
#include <rtc/rtc.hpp>

#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace sfu {

class Loop;

using ClientId = uint64_t;

class Participant {
public:
    Participant(const std::shared_ptr<rtc::PeerConnection>& peerConnection, ClientId clientId);

    void SetAudioTrack(const std::shared_ptr<rtc::Track>& track);

    void AddRemoteTrack(ClientId clientId, const std::shared_ptr<rtc::Track>& track) {
        std::unique_lock guard(TracksMutex_);
        OutgoingTracks_[clientId] = track;
    }

    void RemoveRemoteTrack(ClientId clientId) {
        std::unique_lock guard(TracksMutex_);
        OutgoingTracks_.erase(clientId);
    }

    std::shared_ptr<rtc::Track> GetAudioTrack() {
        return Track_;
    }

    ClientId GetClientId() {
        return ClientId_;
    }

    std::shared_ptr<rtc::PeerConnection> GetConnection() {
        return PeerConnection_;
    }

    std::map<ClientId, std::shared_ptr<rtc::Track>> OutgoingTracks_;

private:
    std::shared_ptr<rtc::Track> Track_;
    std::shared_ptr<rtc::PeerConnection> PeerConnection_;

    ClientId ClientId_;

    std::shared_mutex TracksMutex_;
};

} // namespace sfu
