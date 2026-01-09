#include "participant.hpp"
#include "router.hpp"

namespace sfu {

Participant::Participant(const std::shared_ptr<rtc::PeerConnection>& peerConnection, ClientId clientId)
    : PeerConnection_(peerConnection), ClientId_(clientId)
{ }

void Participant::SetAudioTrack(const std::shared_ptr<rtc::Track>& track) {
    Track_ = track;

    Track_->onMessage([this](rtc::binary message) {
        std::shared_lock lock(TracksMutex_);
        
        for (auto [id, target] : OutgoingTracks_) {
            if (target->isOpen()) {
			    auto rtp = reinterpret_cast<rtc::RtpHeader *>(message.data());
                rtp->setSsrc(this->GetClientId());
                target->send(message);
            }
        }
    }, nullptr);
}

} // namespace sfu
