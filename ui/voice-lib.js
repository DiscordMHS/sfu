export class VoiceClient {
  constructor(config = {}) {
    this.pc = null;
    this.ws = null;

    // Local Media State
    this.localAudioStream = null; 
    this.localVideoStream = null; 
    this.placeholderTrack = null;
    
    // 'none' | 'camera' | 'screen'
    this.videoMode = 'none'; 

    this.isConnecting = false;
    this.token = null;

    this.audioTransceiver = null;
    this.videoTransceiver = null;

    // Configuration
    this.iceServers = config.iceServers || [{ urls: "stun:stun.l.google.com:19302" }];
    this.videoMaxBitrateKbps = config.videoMaxBitrateKbps ?? 3000;
    this.forceServerOfferSetupActive = config.forceServerOfferSetupActive ?? true;

    // Callbacks
    this.onLog = config.onLog || console.log;
    this.onTrack = config.onTrack || (() => {});
    this.onTrackEnded = config.onTrackEnded || (() => {});
    this.onDisconnect = config.onDisconnect || (() => {});
    
    // Receives { type: "mode", ssrc: 123, active: true/false }
    this.onRemoteMode = config.onRemoteMode || (() => {}); 
  }

  /**
   * Main entry point.
   */
  async join(wsUrl, token) {
    if (!wsUrl || !token) throw new Error("URL and Token are required.");

    this.disconnect();
    this.isConnecting = true;
    this.token = token;

    try {
      this._log("Requesting microphone...");
      this.localAudioStream = await navigator.mediaDevices.getUserMedia({ audio: true });

      // Create the 10x10 black placeholder track (saves bandwidth)
      this.placeholderTrack = this._createPlaceholderTrack();
      this.videoMode = 'none';

    } catch (err) {
      this.isConnecting = false;
      throw new Error(`Media Setup Failed: ${err.message}`);
    }

    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(wsUrl);

      const timer = setTimeout(() => {
        if (this.isConnecting) {
          this._cleanup();
          reject(new Error("Connection timed out"));
        }
      }, 5000);

      this.ws.onopen = async () => {
        this._log("WebSocket connected. Starting negotiation.");
        try {
          await this._createPeerConnectionAndSendInitialOffer();
        } catch (e) {
          clearTimeout(timer);
          reject(e);
        }
      };

      this.ws.onmessage = async (event) => {
        const data = JSON.parse(event.data);

        // --- HANDLER: Remote Mode Switching (SSRC based) ---
        if (data.type === "mode") {
            this.onRemoteMode(data);
            return;
        }

        if (data.error || data.type === "error") {
          clearTimeout(timer);
          this.isConnecting = false;
          const errorMsg = data.error || data.message || "Unknown server error";
          this._log(`Server rejected: ${errorMsg}`);
          this._cleanup();
          reject(new Error(errorMsg));
          return;
        }

        if (data.type === "answer") {
          clearTimeout(timer);
          this._log("Received Answer.");
          const fixedSdp = this._fixSdp(data.sdp);
          await this.pc.setRemoteDescription({ type: "answer", sdp: fixedSdp });
          
          this.isConnecting = false;

          // REMOVED: this._sendMode(false); 
          // We no longer send initial inactive state. Receiver defaults to inactive.

          resolve();
          return;
        }

        if (data.type === "offer") {
          await this._handleServerOffer(data);
          return;
        }

        if (data.type === "candidate") {
          if (this.pc && this.pc.remoteDescription) {
            try {
              await this.pc.addIceCandidate({
                candidate: data.candidate,
                sdpMid: data.sdpMid,
                sdpMLineIndex: data.sdpMLineIndex,
              });
            } catch (e) {}
          }
        }
      };

      this.ws.onclose = () => {
        if (this.isConnecting) {
          clearTimeout(timer);
          this.isConnecting = false;
          reject(new Error("WebSocket closed unexpectedly during handshake."));
        } else {
          this._log("WebSocket closed by server.");
          this.disconnect();
        }
      };

      this.ws.onerror = () => {
        if (this.isConnecting) {
          clearTimeout(timer);
          this.isConnecting = false;
          reject(new Error("WebSocket connection error."));
        }
      };
    });
  }

  // --- SIGNALING HELPER ---
  _sendMode(isActive) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.ws.send(JSON.stringify({
            type: "mode",
            active: isActive
        }));
    }
  }

  // --- CAMERA LOGIC ---

  async startCamera() {
    if (!this.pc || !this.videoTransceiver) throw new Error("Not connected");
    if (this.videoMode === 'camera') return;

    if (this.videoMode === 'screen') this._stopCurrentVideoTrack();

    this._log("Starting Camera...");
    const stream = await navigator.mediaDevices.getUserMedia({
      video: { width: { ideal: 1280 }, height: { ideal: 720 }, frameRate: { ideal: 30 } }
    });
    
    this.localVideoStream = stream;
    const track = stream.getVideoTracks()[0];
    track.onended = () => this.stopCamera().catch(() => {});

    await this.videoTransceiver.sender.replaceTrack(track);
    this.videoMode = 'camera';
    
    await this._setVideoMaxBitrate();
    this._sendMode(true); // Explicitly active
  }

  async stopCamera() {
    if (this.videoMode !== 'camera') return;
    this._log("Stopping Camera...");
    await this._revertToPlaceholder();
    this.videoMode = 'none';
    
    this._sendMode(false); // Explicitly inactive
  }

  // --- SCREEN SHARE LOGIC ---

  async startScreenShare() {
    if (!this.pc || !this.videoTransceiver) throw new Error("Not connected");
    if (this.videoMode === 'screen') return;

    if (this.videoMode === 'camera') this._stopCurrentVideoTrack();

    this._log("Starting Screen Share...");
    const stream = await navigator.mediaDevices.getDisplayMedia({ video: true, audio: false });
    
    this.localVideoStream = stream;
    const track = stream.getVideoTracks()[0];
    track.onended = () => this.stopScreenShare().catch(() => {});

    await this.videoTransceiver.sender.replaceTrack(track);
    this.videoMode = 'screen';
    
    await this._setVideoMaxBitrate();
    this._sendMode(true); // Explicitly active
  }

  async stopScreenShare() {
    if (this.videoMode !== 'screen') return;
    this._log("Stopping Screen Share...");
    await this._revertToPlaceholder();
    this.videoMode = 'none';
    
    this._sendMode(false); // Explicitly inactive
  }

  // --- INTERNAL UTILS ---

  _stopCurrentVideoTrack() {
    if (this.localVideoStream) {
      this.localVideoStream.getTracks().forEach(t => t.stop());
      this.localVideoStream = null;
    }
  }

  async _revertToPlaceholder() {
    this._stopCurrentVideoTrack();
    if (this.videoTransceiver && this.placeholderTrack) {
        await this.videoTransceiver.sender.replaceTrack(this.placeholderTrack);
    }
  }

  _createPlaceholderTrack() {
    // 10x10 black canvas running at 1 FPS
    const canvas = document.createElement("canvas");
    canvas.width = 10; canvas.height = 10;
    const ctx = canvas.getContext("2d");
    ctx.fillStyle = "#000"; ctx.fillRect(0,0,10,10);
    const stream = canvas.captureStream(1); 
    const track = stream.getVideoTracks()[0];
    track.enabled = true;
    return track;
  }

  disconnect() {
    if (!this.pc && !this.ws) return;
    this.isConnecting = false;
    this._cleanup();
    this._log("Disconnected.");
    this.onDisconnect(); 
  }

  getDebugInfo() {
    const info = { sending: [], receiving: [] };
    if (!this.pc) return info;
    for (const t of this.pc.getTransceivers()) {
      const mid = t.mid ?? "?";
      if (t.sender?.track) info.sending.push(`${mid}:${t.sender.track.kind}=${this.videoMode}`);
      if (t.receiver?.track) info.receiving.push(`${mid}:${t.receiver.track.kind}`);
    }
    return info;
  }

  async getReceiverStats() {
    if (!this.pc) return [];
    return [];
  }

  _log(msg) {
    this.onLog(`[${new Date().toLocaleTimeString()}] ${msg}`);
  }

  _cleanup() {
    this._stopCurrentVideoTrack();
    if (this.placeholderTrack) {
        this.placeholderTrack.stop();
        this.placeholderTrack = null;
    }
    if (this.localAudioStream) {
        this.localAudioStream.getTracks().forEach(t => t.stop());
        this.localAudioStream = null;
    }

    if (this.ws) {
      this.ws.onclose = null; 
      this.ws.onerror = null;
      this.ws.onmessage = null;
      this.ws.close(1000);
      this.ws = null;
    }

    if (this.pc) {
      this.pc.onicecandidate = null;
      this.pc.ontrack = null;
      this.pc.close();
      this.pc = null;
    }

    this.audioTransceiver = null;
    this.videoTransceiver = null;
  }

  _fixSdp(sdp) {
    if (sdp.indexOf("a=group:BUNDLE") !== -1) return sdp;
    const midRegex = /a=mid:([^\r\n\s]+)/g;
    const mids = [];
    let match;
    while ((match = midRegex.exec(sdp)) !== null) {
      mids.push(match[1]);
    }
    if (mids.length < 2) return sdp;
    const bundleLine = "a=group:BUNDLE " + mids.join(" ") + "\r\n";
    const firstMediaIdx = sdp.search(/\nm=/);
    if (firstMediaIdx === -1) return sdp;
    return sdp.slice(0, firstMediaIdx + 1) + bundleLine + sdp.slice(firstMediaIdx + 1);
  }

  async _createPeerConnectionAndSendInitialOffer() {
    this.pc = new RTCPeerConnection({ iceServers: this.iceServers });

    this.pc.ontrack = (e) => {
      const mid = e.transceiver?.mid ?? "(no-mid)";
      const kind = e.track.kind; 
      
      this.onTrack(e.streams[0], { mid, kind, track: e.track });
      
      e.track.onended = () => {
        this._log(`Track ended: ${kind} ${e.track.id}`);
        this.onTrackEnded({ mid, kind, track: e.track });
      };
    };

    this.pc.onicecandidate = ({ candidate }) => {
      if (candidate && this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.ws.send(JSON.stringify({
          type: "candidate",
          candidate: candidate.candidate,
          sdpMid: candidate.sdpMid,
          sdpMLineIndex: candidate.sdpMLineIndex,
        }));
      }
    };

    // 1. Add Mic
    if (this.localAudioStream) {
        this.audioTransceiver = this.pc.addTransceiver(
            this.localAudioStream.getAudioTracks()[0], 
            { direction: "sendonly", streams: [this.localAudioStream] }
        );
    }

    // 2. Add Placeholder Video
    if (this.placeholderTrack) {
        const stream = new MediaStream([this.placeholderTrack]);
        this.videoTransceiver = this.pc.addTransceiver(
            this.placeholderTrack, 
            { direction: "sendonly", streams: [stream] }
        );
    }

    await this._applyVideoCodecPreferences(); 
    await this._setVideoMaxBitrate();

    const offer = await this.pc.createOffer();
    await this.pc.setLocalDescription(offer);

    this.ws.send(JSON.stringify({
      type: "offer",
      token: this.token,
      sdp: this.pc.localDescription.sdp,
    }));
  }

  async _applyVideoCodecPreferences() {
    if (!this.videoTransceiver?.setCodecPreferences) return;
    const caps = RTCRtpSender.getCapabilities?.("video");
    if (!caps?.codecs) return;
    const vp8Codecs = caps.codecs.filter((c) => (c.mimeType || "").toLowerCase() === "video/vp8");
    if (vp8Codecs.length) this.videoTransceiver.setCodecPreferences(vp8Codecs);
  }

  async _setVideoMaxBitrate() {
    const sender = this.videoTransceiver?.sender;
    if (!sender?.getParameters) return;
    const p = sender.getParameters();
    if (!p.encodings) p.encodings = [{}];
    p.encodings[0].maxBitrate = this.videoMaxBitrateKbps * 1000;
    try { await sender.setParameters(p); } catch (e) {}
  }

  async _handleServerOffer(data) {
    if (!this.pc) return;

    // 1. Handle the SDP Negotiation
    this._log("Renegotiating (Server Offer)...");
    let sdp = this._fixSdp(data.sdp);
    await this.pc.setRemoteDescription({ type: "offer", sdp: sdp });

    const answer = await this.pc.createAnswer();
    let answerSdp = answer.sdp;
    
    // DTLS Setup Fix
    if (this.forceServerOfferSetupActive) {
      answerSdp = answerSdp.replace(/a=setup:active/g, "a=setup:passive");
    }

    const fixedAnswer = { type: "answer", sdp: answerSdp };
    await this.pc.setLocalDescription(fixedAnswer);

    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(fixedAnswer));
      
      // -----------------------------------------------------------
      // NEW: Re-broadcast our video state immediately after answering
      // -----------------------------------------------------------
      // Since the server just updated our connections (likely added a new peer),
      // we need to tell that new peer if our video is currently ON or OFF.
      
      const isActive = (this.videoMode === 'camera' || this.videoMode === 'screen');
      this._sendMode(isActive); 
      
      this._log(`Syncing video state (Active=${isActive}) after renegotiation.`);
    }
  }
}
