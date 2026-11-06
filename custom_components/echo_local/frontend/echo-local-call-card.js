// Drop In on an Echo Show from a dashboard (docs/video-dropin.md): the Echo's picture and sound here, this device's
// microphone there.  custom_components/echo_local/call.py is the other end of the websocket (its comment has the frames).
//
//   type: custom:echo-local-call-card
//   entity: camera.echo_show_5_5695c4_camera      # Echo Local's camera entity of that Echo
//
// The picture: H.264 through WebCodecs (Chrome, Edge, Firefox 130+, Safari 16.4+, the Companion app's WebView), drawn on
// a canvas as it comes: a few hundred ms behind, where HLS is seconds.  Sound: 16 kHz PCM both ways, through
// AudioWorklets (a ScriptProcessor where there are none).  WebCodecs and the microphone need a secure page (https, or
// the app); over plain http the call is the Echo's sound only and says so.
// The sound's two ends, as plain classes: run in an AudioWorklet where there is one, else on the page's own thread
// through a ScriptProcessorNode.  AudioWorklet exists only on secure pages: over plain http Firefox and Chrome both
// leave ctx.audioWorklet undefined ("No sound here: can't access property addModule"), so the Echo's side would not
// play at all.  The context runs at the browser's own rate and these resample to the call's 16 kHz: Firefox refuses to
// connect a microphone to a context of another rate (NotSupportedError), the call was listen-only there.
function core() {
  // The Echo's frames, behind a jitter buffer: they come over TCP through Home Assistant, beside the picture's key
  // frames, in bursts.  Played the moment each came (the first version), every late frame was a hole: the wave cut to
  // zero and back, a pop, many a second.  So: PRE frames (60 ms) before it plays; when it runs dry it fades out over
  // FADE and waits for PRE again, then fades in; more than MAX waiting (the two clocks drift, a stall caught up) drops
  // one frame at a time, crossfaded, never a jump of hundreds of ms.  16 kHz up to the output's rate: linear.
  const PRE = 3, MAX = 8, FADE = 80;                     // frames (20 ms), frames, samples at 16 kHz (5 ms)
  class PlayerCore {
    constructor(rate) { this.q = []; this.cur = null; this.at = 0; this.g = 0; this.on = false;
      this.step = 16000 / rate; this.ph = 0; this.prev = 0; this.last = 0; this.dg = 1 / (FADE / this.step); }
    push(f) { this.q.push(f); }
    next() {                                             // the next frame; one dropped, crossfaded, when too many wait
      let f = this.q.shift() || null;
      if (f && this.q.length > MAX) {
        const n = this.q.shift(), x = new Int16Array(f.length);
        for (let i = 0; i < f.length; i++) { const t = i / f.length; x[i] = f[i] * (1 - t) + n[i] * t; }
        f = x;
      }
      return f;
    }
    fill(o) {
      if (!this.on && this.q.length >= PRE) this.on = true;
      for (let i = 0; i < o.length; i++) {
        if (this.on) {
          this.ph += this.step;
          while (this.ph >= 1) {                         // the next 16 kHz sample is due
            if (!this.cur || this.at >= this.cur.length) { this.cur = this.next(); this.at = 0; }
            if (!this.cur) { this.on = false; this.prev = this.last; this.ph = 0; break; }
            this.prev = this.last; this.last = this.cur[this.at++] / 32768; this.ph -= 1;
          }
        }
        // dry: the last sample fades out, no step to zero
        this.g = this.on ? Math.min(1, this.g + this.dg) : Math.max(0, this.g - this.dg);
        o[i] = (this.prev + (this.last - this.prev) * this.ph) * this.g;
      }
    }
  }
  // this side's microphone in 20 ms frames of 16 kHz: each output sample the mean of the input samples since the last
  // (3 at 48 kHz): a crude low-pass, enough for a voice the browser already cleaned up
  class MicCore {
    constructor(rate, out) { this.buf = new Int16Array(320); this.n = 0; this.step = 16000 / rate; this.ph = 0;
      this.acc = 0; this.k = 0; this.out = out; }
    feed(x) {
      for (let i = 0; i < x.length; i++) {
        this.acc += x[i]; this.k++; this.ph += this.step;
        if (this.ph < 1) continue;
        this.ph -= 1;
        this.buf[this.n++] = Math.max(-32768, Math.min(32767, Math.round(this.acc / this.k * 32767)));
        this.acc = 0; this.k = 0;
        if (this.n === 320) { this.out(this.buf.buffer.slice(0)); this.n = 0; }
      }
    }
  }
  return { PlayerCore, MicCore };
}
const WORKLET = `
const { PlayerCore, MicCore } = (${core.toString()})();
class Player extends AudioWorkletProcessor {
  constructor() { super(); this.c = new PlayerCore(sampleRate); this.port.onmessage = (e) => this.c.push(e.data); }
  process(_, outs) { this.c.fill(outs[0][0]); return true; }
}
class Mic extends AudioWorkletProcessor {
  constructor() { super(); this.c = new MicCore(sampleRate, (b) => this.port.postMessage(b)); }
  process(ins) { const x = ins[0] && ins[0][0]; if (x) this.c.feed(x); return true; }
}
registerProcessor('echo-local-player', Player);
registerProcessor('echo-local-mic', Mic);
`;

class EchoLocalCallCard extends HTMLElement {
  setConfig(config) {
    if (!config.entity) throw new Error('entity: the Echo\'s camera entity (Echo Local)');
    this.config = config;
    if (!this.shadowRoot) this.attachShadow({ mode: 'open' });
    this.render();
  }

  set hass(h) { this._hass = h; if (!this.rendered) this.render(); }
  getCardSize() { return 5; }
  static getStubConfig(hass) {
    const e = Object.keys(hass.states).find((k) => k.startsWith('camera.') && hass.states[k].attributes.lens_covered !== undefined);
    return { entity: e || 'camera.' };
  }

  render() {
    if (!this._hass || !this.config) return;
    this.rendered = true;
    const name = (this._hass.states[this.config.entity] || { attributes: {} }).attributes.friendly_name || this.config.entity;
    this.shadowRoot.innerHTML = `
      <style>
        ha-card { overflow: hidden; }
        .v { position: relative; background: #000; aspect-ratio: 16 / 9; display: flex; align-items: center; justify-content: center; }
        canvas { width: 100%; height: 100%; object-fit: contain; display: block; transition: filter 1s linear; }
        /* the frosted start, as stock's Drop In: an Echo that did not ring is seen blurred for its first seconds (3 s,
           then 1 s clearing), so whoever is dropped in on has a moment; one that rang and was answered, at once */
        canvas.frost { filter: blur(28px) brightness(1.08) saturate(.8); transition: none; }
        .msg { position: absolute; color: #ddd; font: 14px var(--paper-font-body1_-_font-family, sans-serif); text-align: center; padding: 0 16px; }
        .bar { display: flex; gap: 8px; align-items: center; padding: 10px 12px; }
        .name { flex: 1; font-weight: 500; }
        button { border: 0; border-radius: 18px; padding: 8px 16px; font: inherit; cursor: pointer; color: #fff; background: var(--success-color, #2e7d32); }
        button.end { background: var(--error-color, #c62828); }
        /* the microphone's switch over the picture, bottom left: muted at the start of every call, so nothing of this
           room reaches the Echo until it is pressed */
        .ctl { position: absolute; left: 12px; bottom: 12px; display: flex; gap: 8px; }
        .ctl button { display: flex; align-items: center; gap: 6px; padding: 8px 14px;
                      background: rgba(0, 0, 0, .6); color: #fff; backdrop-filter: blur(4px); }
        .ctl button.on { background: var(--success-color, #2e7d32); }
        .ctl button ha-icon { --mdc-icon-size: 20px; }
        /* this side's own picture while it goes to the Echo's screen: top right, mirrored as a mirror is */
        video.self { position: absolute; right: 10px; top: 10px; width: 22%; border-radius: 8px; transform: scaleX(-1); background: #000; }
        video.self[hidden] { display: none; }
        button[hidden] { display: none !important; }
      </style>
      <ha-card>
        <div class="v"><canvas width="1280" height="720"></canvas><div class="msg">Drop In to see and talk to ${name}</div>
          <video class="self" muted playsinline hidden></video>
          <div class="ctl"><button class="mute" hidden><ha-icon icon="mdi:microphone-off"></ha-icon><span>Unmute</span></button>
            <button class="cam" hidden><ha-icon icon="mdi:video-off"></ha-icon><span>Camera</span></button></div></div>
        <div class="bar"><span class="name">${name}</span>
          <button class="go">Drop In</button><button class="end" hidden>Hang up</button></div>
      </ha-card>`;
    const $ = (s) => this.shadowRoot.querySelector(s);
    this.$ = { canvas: $('canvas'), msg: $('.msg'), go: $('.go'), end: $('.end'), mute: $('.mute'), cam: $('.cam'), self: $('video.self') };
    this.$.cam.onclick = () => (this.enc ? this.camOff() : this.camOn());
    this.$.go.onclick = () => this.start();
    this.$.end.onclick = () => this.stop('');
    this.$.mute.onclick = () => this.setMuted(!this.muted);
  }

  setMuted(m) {
    this.muted = m;
    this.$.mute.classList.toggle('on', !m);
    this.$.mute.querySelector('ha-icon').setAttribute('icon', m ? 'mdi:microphone-off' : 'mdi:microphone');
    this.$.mute.querySelector('span').textContent = m ? 'Unmute' : 'Mute';
  }

  say(t) { this.$.msg.textContent = t; this.$.msg.hidden = !t; }
  live(on) {
    this.$.go.hidden = on; this.$.end.hidden = !on; this.$.mute.hidden = !on;
    // the camera only where the Echo shows a call's picture (an Echo Show with hassmic's video.c: the entity says so)
    const a = (this._hass.states[this.config.entity] || { attributes: {} }).attributes;
    this.$.cam.hidden = !on || !a.shows_video;
  }

  // This side's camera on the Echo's screen: off at the start of every call, as the microphone.  H.264 Baseline in
  // Annex B through WebCodecs (its key frames carry SPS/PPS: cam.c's player takes them inline), 640x360 at 15 fps,
  // ~500 kbit/s, a key frame a second (the Echo's player starts at one); frames skipped while the encoder or the
  // websocket is behind, never queued up behind the call's sound
  async camOn() {
    if (!('VideoEncoder' in window) || !('VideoFrame' in window)) { this.say('This browser cannot send its camera (no WebCodecs encoder)'); return; }
    const cfg = { codec: 'avc1.42E01F', width: 640, height: 360, bitrate: 500000, framerate: 15, latencyMode: 'realtime', avc: { format: 'annexb' } };
    try {
      const ok = await VideoEncoder.isConfigSupported(cfg);
      if (!ok.supported) { this.say('This browser cannot encode H.264 for the Echo'); return; }
      this.camStream = await navigator.mediaDevices.getUserMedia({ video: { width: 640, height: 360, frameRate: 15 } });
    } catch (e) { this.say('No camera here (' + e.name + ')'); return; }
    if (!this.ws) { this.camStream.getTracks().forEach((t) => t.stop()); this.camStream = null; return; }   // hung up meanwhile
    const v = this.$.self; v.srcObject = this.camStream; v.hidden = false; await v.play().catch(() => {});
    this.enc = new VideoEncoder({
      output: (chunk) => {
        if (!this.ws || this.ws.readyState !== 1 || this.state !== 'connected') return;
        const out = new Uint8Array(10 + chunk.byteLength); out[0] = 86; out[1] = chunk.type === 'key' ? 2 : 3;   // 'V'
        new DataView(out.buffer).setBigUint64(2, BigInt(Math.max(0, Math.round(chunk.timestamp))), true);
        chunk.copyTo(out.subarray(10));
        this.ws.send(out);
      },
      error: (e) => { this.say('The camera stopped: ' + e.message); this.camOff(); },
    });
    this.enc.configure(cfg);
    // Each frame drawn upright into the encoder's 640x360 first: a phone's camera hands VideoFrame(video) the sensor's
    // pixels with the turn only as metadata (WebCodecs' rotation), which the encoder leaves out, so the Echo showed a
    // phone's picture on its side; drawImage applies it.  A portrait camera goes in whole, with bars beside it.
    const cv = document.createElement('canvas'); cv.width = cfg.width; cv.height = cfg.height;
    const g = cv.getContext('2d', { alpha: false });
    let n = 0; const t0 = performance.now();
    this.camTimer = setInterval(() => {
      if (!this.enc || this.enc.state !== 'configured' || v.readyState < 2 || !v.videoWidth) return;
      if (this.enc.encodeQueueSize > 2 || (this.ws && this.ws.bufferedAmount > 1 << 20)) return;   // behind: skip one
      const k = Math.min(cv.width / v.videoWidth, cv.height / v.videoHeight), w = v.videoWidth * k, h = v.videoHeight * k;
      g.fillStyle = '#000'; g.fillRect(0, 0, cv.width, cv.height);
      g.drawImage(v, (cv.width - w) / 2, (cv.height - h) / 2, w, h);
      const f = new VideoFrame(cv, { timestamp: Math.round((performance.now() - t0) * 1000) });
      this.enc.encode(f, { keyFrame: n++ % 15 === 0 }); f.close();
    }, 1000 / 15);
    this.$.cam.classList.add('on');
    this.$.cam.querySelector('ha-icon').setAttribute('icon', 'mdi:video');
  }

  camOff() {
    clearInterval(this.camTimer); this.camTimer = null;
    if (this.enc && this.enc.state !== 'closed') this.enc.close();
    this.enc = null;
    if (this.camStream) this.camStream.getTracks().forEach((t) => t.stop());
    this.camStream = null; this.$.self.srcObject = null; this.$.self.hidden = true;
    this.$.cam.classList.remove('on');
    this.$.cam.querySelector('ha-icon').setAttribute('icon', 'mdi:video-off');
  }

  async start() {
    if (this.ws) return;
    this.live(true); this.setMuted(true); this.say('Calling…');
    try {
      this.ctx = new AudioContext();
      if (this.ctx.audioWorklet) {
        await this.ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([WORKLET], { type: 'application/javascript' })));
        const n = new AudioWorkletNode(this.ctx, 'echo-local-player');
        this.player = { push: (f) => n.port.postMessage(f) };
        n.connect(this.ctx.destination);
      } else {                                                   // plain http: no worklets, the page's own thread
        const { PlayerCore } = core(), c = new PlayerCore(this.ctx.sampleRate), n = this.ctx.createScriptProcessor(1024, 0, 1);
        n.onaudioprocess = (e) => c.fill(e.outputBuffer.getChannelData(0));
        this.player = { push: (f) => c.push(f) };
        n.connect(this.ctx.destination); this.nodes = [n];
      }
    } catch (e) { this.stop('No sound here: ' + e.message); return; }
    this.mic = null;
    if (navigator.mediaDevices && navigator.mediaDevices.getUserMedia) {
      try {
        this.stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true } });
        const src = this.ctx.createMediaStreamSource(this.stream);
        if (this.ctx.audioWorklet) {
          this.mic = new AudioWorkletNode(this.ctx, 'echo-local-mic');
          this.mic.port.onmessage = (e) => this.send(e.data);
        } else {
          const { MicCore } = core(), c = new MicCore(this.ctx.sampleRate, (b) => this.send(b));
          this.mic = this.ctx.createScriptProcessor(1024, 1, 1);
          this.mic.onaudioprocess = (e) => c.feed(e.inputBuffer.getChannelData(0));
          this.mic.connect(this.ctx.destination);                // it runs only when connected; its output is silence
        }
        src.connect(this.mic);
      } catch (e) { this.listenOnly = 'Your microphone is off for this page (' + e.name + '): listening only'; }
    } else if (!window.isSecureContext)
      this.listenOnly = 'Plain http: the Echo\'s sound only. Its picture and your microphone need Home Assistant over https, or the app';
    else this.listenOnly = 'This browser has no microphone for the page: listening only';
    if (!this.mic) this.$.mute.hidden = true;                    // listening only: nothing to switch
    if (!this.mic) this.keep = setInterval(() => this.send(new ArrayBuffer(640)), 20);   // silence: the call's keepalive
    this.decoderStart();
    const signed = await this._hass.callWS({ type: 'auth/sign_path', path: `/api/echo_local/call/${this.config.entity}`, expires: 30 });
    const url = new URL(signed.path, location.href); url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
    const ws = this.ws = new WebSocket(url.href); ws.binaryType = 'arraybuffer';
    ws.onmessage = (e) => this.onMessage(e.data);
    ws.onclose = () => { if (this.ws === ws) this.stop(this.ended || 'The call ended'); };
  }

  send(buf) {
    if (!this.ws || this.ws.readyState !== 1 || this.state !== 'connected') return;
    const out = new Uint8Array(641); out[0] = 65;                // 'A'
    if (!this.muted) out.set(new Uint8Array(buf), 1);
    this.ws.send(out);
  }

  onMessage(d) {
    if (typeof d === 'string') {
      const m = JSON.parse(d);
      if (m.error) { this.ended = 'The Echo said no: ' + m.error; return; }
      if (m.state) {
        this.state = m.state;
        if (m.state === 'ringing') { this.rang = true; this.$.canvas.classList.remove('frost'); this.say('Ringing: someone presses the button on the Echo…'); }
        else if (m.state === 'connected') this.say(this.listenOnly || this.noVideo || (this.gotPicture ? '' : 'Connected: no picture (the Echo\'s camera is off or covered)'));
        else if (m.state === 'idle') this.ended = 'The call ended';
      }
      return;
    }
    const b = new Uint8Array(d);
    if (b[0] === 65) this.player.push(new Int16Array(d.slice(1)));            // 'A'
    else if (b[0] === 86) this.picture(b[1], new DataView(d).getBigUint64(2, true), b.subarray(10));   // 'V'
  }

  decoderStart() {
    this.config_ = null; this.gotPicture = false; this.rang = false;
    this.$.canvas.classList.add('frost');                        // until it rings, or 3 s after the first picture
    if (!('VideoDecoder' in window)) { this.noVideo = 'This browser cannot show the picture (no WebCodecs)'; return; }
    const g = this.$.canvas.getContext('2d');
    this.decoder = new VideoDecoder({
      output: (f) => {
        if (this.$.canvas.width !== f.displayWidth) { this.$.canvas.width = f.displayWidth; this.$.canvas.height = f.displayHeight; }
        g.drawImage(f, 0, 0); f.close();
        if (!this.gotPicture) {
          this.gotPicture = true; this.say(this.listenOnly || '');
          clearTimeout(this.frostTimer);
          if (!this.rang) this.frostTimer = setTimeout(() => this.$.canvas.classList.remove('frost'), 3000);
        }
      },
      error: (e) => { this.noVideo = 'The picture failed: ' + e.message; },
    });
    this.decoder.configure({ codec: 'avc1.42E01F', optimizeForLatency: true });   // Annex B: no description
  }

  picture(type, pts, data) {
    if (!this.decoder || this.decoder.state !== 'configured') return;
    if (type === 1) { this.config_ = data.slice(); return; }     // SPS/PPS: before every key frame
    if (type !== 2 && !this.keyed) return;                       // from a key frame on
    let bytes = data;
    if (type === 2 && this.config_) { bytes = new Uint8Array(this.config_.length + data.length); bytes.set(this.config_); bytes.set(data, this.config_.length); }
    this.keyed = true;
    if (this.decoder.decodeQueueSize > 4) return;                // behind: drop rather than lag
    this.decoder.decode(new EncodedVideoChunk({ type: type === 2 ? 'key' : 'delta', timestamp: Number(pts), data: bytes }));
  }

  stop(why) {
    const ws = this.ws; this.ws = null;
    if (ws) { try { if (ws.readyState === 1) ws.send(JSON.stringify({ hangup: true })); ws.close(); } catch (e) { /* gone */ } }
    clearInterval(this.keep);
    this.camOff();
    if (this.stream) this.stream.getTracks().forEach((t) => t.stop());
    if (this.decoder && this.decoder.state !== 'closed') this.decoder.close();
    if (this.ctx) this.ctx.close();
    clearTimeout(this.frostTimer); this.$.canvas.classList.remove('frost');
    this.stream = this.ctx = this.decoder = this.mic = this.player = this.noVideo = null; this.nodes = []; this.keyed = false; this.state = ''; this.ended = ''; this.listenOnly = '';
    this.live(false);
    this.say(why || 'Drop In to see and talk to the Echo');
  }

  disconnectedCallback() { if (this.ws) this.stop(''); }
}

// once Home Assistant's root element is, through its registry of then (echo-local-alarms-card.js says why: Firefox)
customElements.whenDefined('home-assistant').then(() => {
  if (!window.customElements.get('echo-local-call-card')) window.customElements.define('echo-local-call-card', EchoLocalCallCard);
});
window.customCards = window.customCards || [];
if (!window.customCards.some((c) => c.type === 'echo-local-call-card'))
  window.customCards.push({ type: 'echo-local-call-card', name: 'Echo Drop In', description: 'See and talk to an Echo Show (Echo Local)' });
