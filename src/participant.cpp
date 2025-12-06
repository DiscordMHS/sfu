#include "participant.hpp"

namespace sfu {

Participant::Participant(const std::shared_ptr<rtc::PeerConnection>& peerConnection)
    : PeerConnection_(peerConnection)
{ }

void Participant::SetAudioTrack(const std::shared_ptr<rtc::Track>& track) {
    Track_ = track;

    Track_->onMessage([this](rtc::binary message) {
        std::shared_lock lock(TracksMutex_);
        
        for (auto& [id, target] : OutgoingTracks_) {
            if (target->isOpen()) {
                //std::cout << "Sending" << std::endl;
                target->send(message);
            }
        }
    }, nullptr);
}

} // namespace sfu
