export class VoiceClient {
    constructor(config = {}) {
        this.pc = null;
        this.ws = null;
        this.localStream = null;
        this.isConnecting = false;

        // Configuration
        this.iceServers = config.iceServers || [{ urls: "stun:stun.l.google.com:19302" }];
        
        // Callbacks
        this.onLog = config.onLog || console.log;
        this.onTrack = config.onTrack || (() => {});
        this.onDisconnect = config.onDisconnect || (() => {});
    }

    /**
     * Initializes mic, connects to WS, sends Offer + Token.
     * Resolves on "answer", Rejects on "error" message or timeout.
     */
    async join(wsUrl, token) {
        if (!wsUrl || !token) throw new Error("URL and Token are required.");
        
        // Ensure we are clean before starting
        this.disconnect();
        
        this.isConnecting = true;

        // 1. Get Microphone
        try {
            this._log("Requesting microphone access...");
            this.localStream = await navigator.mediaDevices.getUserMedia({ audio: true });
        } catch (err) {
            this.isConnecting = false;
            throw new Error(`Microphone denied: ${err.message}`);
        }

        return new Promise((resolve, reject) => {
            this.ws = new WebSocket(wsUrl);

            // Safety timeout: 5 seconds to connect
            const timer = setTimeout(() => {
                if (this.isConnecting) {
                    this._cleanup();
                    reject(new Error("Connection timed out"));
                }
            }, 5000);

            this.ws.onopen = async () => {
                this._log("WebSocket connected. Starting negotiation.");
                try {
                    await this._createPeerConnection(token);
                } catch (e) {
                    clearTimeout(timer);
                    reject(e);
                }
            };

            this.ws.onmessage = async (event) => {
                const data = JSON.parse(event.data);

                // --- 1. HANDLE SERVER ERROR (INVALID TOKEN) ---
                if (data.error || data.type === 'error') {
                    clearTimeout(timer);
                    this.isConnecting = false;
                    const errorMsg = data.error || data.message || "Unknown server error";
                    this._log(`Server rejected connection: ${errorMsg}`);
                    
                    this._cleanup(); 
                    reject(new Error(errorMsg));
                    return;
                }

                // --- 2. HANDLE SUCCESSFUL CONNECTION ---
                if (data.type === 'answer') {
                    clearTimeout(timer);
                    this._log("Received Answer. Connection established.");
                    await this.pc.setRemoteDescription(data);
                    this.isConnecting = false;
                    resolve(); 
                } 
                
                // --- 3. HANDLE SERVER-SIDE OFFERS ---
                else if (data.type === 'offer') {
                    await this._handleRenegotiation(data);
                } 
                
                // --- 4. ICE CANDIDATES ---
                else if (data.type === 'candidate') {
                    if (this.pc && this.pc.remoteDescription) {
                        try { await this.pc.addIceCandidate(data); } catch(e){}
                    }
                }
            };

            this.ws.onclose = () => {
                if (this.isConnecting) {
                    clearTimeout(timer);
                    this.isConnecting = false;
                    reject(new Error("WebSocket closed unexpectedly during handshake."));
                } else {
                    this._log("WebSocket closed.");
                    this.onDisconnect();
                }
            };

            this.ws.onerror = () => {
                if (this.isConnecting) {
                    clearTimeout(timer);
                    this.isConnecting = false;
                    reject(new Error("WebSocket connection error (Check URL)."));
                }
            };
        });
    }

    /**
     * Manually disconnects the client.
     * Closes WebSocket, invalidates PeerConnection, and stops Microphone.
     */
    disconnect() {
        this.isConnecting = false;
        this._cleanup();
        this._log("Disconnected by user.");
    }

    // --- INTERNAL HELPERS ---

    _log(msg) {
        this.onLog(`[${new Date().toLocaleTimeString()}] ${msg}`);
    }

    _cleanup() {
        // 1. Close WebSocket
        if (this.ws) {
            // Remove listeners so the manual close doesn't trigger 'onDisconnect' 
            // or reject any pending Promises unexpectedly.
            this.ws.onclose = null;
            this.ws.onerror = null;
            this.ws.onmessage = null;
            this.ws.onopen = null;

            if (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING) {
                this.ws.close(1000, "Normal Closure");
            }
            this.ws = null;
        }

        // 2. Close PeerConnection
        if (this.pc) {
            this.pc.onicecandidate = null;
            this.pc.ontrack = null;
            this.pc.close();
            this.pc = null;
        }

        // 3. Stop Microphone (releases hardware)
        if (this.localStream) {
            this.localStream.getTracks().forEach(t => t.stop());
            this.localStream = null;
        }
    }

    async _createPeerConnection(token) {
        this.pc = new RTCPeerConnection({ iceServers: this.iceServers });

        this.localStream.getTracks().forEach(track => {
            this.pc.addTransceiver(track, { direction: 'sendonly', streams: [this.localStream] });
        });

        this.pc.ontrack = (e) => {
            this._log(`Track received: ${e.track.id}`);
            this.onTrack(e.streams[0], e.track.id);
        };

        this.pc.onicecandidate = ({ candidate }) => {
            if (candidate && this.ws && this.ws.readyState === WebSocket.OPEN) {
                this.ws.send(JSON.stringify({ type: "candidate", candidate: candidate.candidate, sdpMid: candidate.sdpMid }));
            }
        };

        const offer = await this.pc.createOffer();
        await this.pc.setLocalDescription(offer);

        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({
                type: "offer",
                token: token,
                sdp: this.pc.localDescription.sdp
            }));
        }
    }

    async _handleRenegotiation(data) {
        if (!this.pc) return;
        this._log("Renegotiating (Server Offer)...");
        await this.pc.setRemoteDescription(data);
        const answer = await this.pc.createAnswer();
        
        let sdp = answer.sdp.replace(/a=setup:active/g, "a=setup:passive");
        const fixedAnswer = { type: "answer", sdp };
        
        await this.pc.setLocalDescription(fixedAnswer);
        
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(fixedAnswer));
        }
    }
}
