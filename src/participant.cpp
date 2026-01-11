#include "participant.hpp"
#include "router.hpp"
#include "rtc/frameinfo.hpp"

namespace sfu {

Participant::Participant(const std::shared_ptr<rtc::PeerConnection>& peerConnection, ClientId clientId)
    : PeerConnection_(peerConnection), ClientId_(clientId)
{ }

void Participant::SetTracks(const std::array<std::shared_ptr<rtc::Track>, 2>& tracks) {
    Tracks_ = tracks;

    Tracks_[0]->onMessage([this](rtc::binary message) {
        std::shared_lock lock(TracksMutex_);
        
        for (auto [id, target] : OutgoingTracks_) {
            if (target[0]->isOpen()) {
                auto rtp = reinterpret_cast<rtc::RtpHeader *>(message.data());
                rtp->setSsrc(target[0]->description().getSSRCs()[0]);
                target[0]->send(message);
            }
        }
    }, nullptr);

    Tracks_[1]->onMessage([this](rtc::binary videoMessage) {
        std::shared_lock lock(TracksMutex_);

        for (auto [id, target] : OutgoingTracks_) {
            if (target[1]->isOpen()) {
                auto rtp = reinterpret_cast<rtc::RtpHeader *>(videoMessage.data());
                rtp->setSsrc(target[1]->description().getSSRCs()[0]);
                target[1]->send(videoMessage);
            }
        }
    }, nullptr);
    Tracks_[1]->requestKeyframe();
}

} // namespace sfu
