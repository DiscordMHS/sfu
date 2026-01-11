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

    void SetTracks(const std::array<std::shared_ptr<rtc::Track>, 2>& tracks);

    void AddRemoteTracks(ClientId clientId, const std::array<std::shared_ptr<rtc::Track>, 2>& tracks) {
        std::unique_lock guard(TracksMutex_);
        OutgoingTracks_[clientId] = tracks;
    }

    void CloseRemoteTracks() {
        std::unique_lock guard(TracksMutex_);
        for (auto& [id, tracks] : OutgoingTracks_) {
            for (auto& track : tracks) {
                track->close();
            }
        }
        OutgoingTracks_.clear();
    }

    void RemoveRemoteTracks(ClientId clientId) {
        std::unique_lock guard(TracksMutex_);
        for (auto& track : OutgoingTracks_[clientId]) {
            track->close();
        }
        OutgoingTracks_.erase(clientId);
    }

    const auto& GetTracks() {
        return Tracks_;
    }

    const auto& GetOutgoingTracks() {
        return OutgoingTracks_;
    }

    ClientId GetClientId() {
        return ClientId_;
    }

    std::shared_ptr<rtc::PeerConnection> GetConnection() {
        return PeerConnection_;
    }

    std::map<ClientId, std::array<std::shared_ptr<rtc::Track>, 2>> OutgoingTracks_;

private:
    std::array<std::shared_ptr<rtc::Track>, 2> Tracks_;
    std::vector<std::byte> CachedKeyFrame_;

    std::shared_ptr<rtc::PeerConnection> PeerConnection_;

    ClientId ClientId_;

    std::shared_mutex TracksMutex_;
};

} // namespace sfu
