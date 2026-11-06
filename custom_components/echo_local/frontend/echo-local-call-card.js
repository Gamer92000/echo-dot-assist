// Drop In on an Echo Show from a dashboard (docs/video-dropin.md): the Echo's picture and sound here, this device's
// microphone there.  custom_components/echo_local/call.py is the other end of the websocket (its comment has the frames).
//
//   type: custom:echo-local-call-card
//   entity: camera.echo_show_5_5695c4_camera      # Echo Local's camera entity of that Echo
//
// The picture: H.264 through WebCodecs (Chrome, Edge, Safari 16.4+, the Companion app's WebView), drawn on a canvas as
// it comes: a few hundred ms behind, where HLS is seconds.  Sound: 16 kHz PCM both ways through AudioWorklets.  The
// microphone needs a secure page (https, or the app); without one the call is listen-only and says so.
const WORKLET = `
// The Echo's frames, behind a jitter buffer: they come over TCP through Home Assistant, beside the picture's key frames,
// in bursts.  Played the moment each came (the first version), every late frame was a hole: the wave cut to zero and
// back, a pop, many a second.  So: PRE frames (60 ms) before it plays; when it runs dry it fades out over FADE samples
// and waits for PRE again, then fades in; more than MAX waiting (the two clocks drift, a stall caught up) drops one
// frame at a time, crossfaded, never a jump of hundreds of ms.
class Player extends AudioWorkletProcessor {
  constructor() { super(); this.q = []; this.cur = null; this.at = 0; this.g = 0; this.on = false; this.last = 0;
    this.port.onmessage = (e) => { this.q.push(e.data); }; }
  next() {                                               // the next frame; one dropped, crossfaded, when too many wait
    let f = this.q.shift() || null;
    if (f && this.q.length > MAX) {
      const n = this.q.shift(), x = new Int16Array(f.length);
      for (let i = 0; i < f.length; i++) { const t = i / f.length; x[i] = f[i] * (1 - t) + n[i] * t; }
      f = x;
    }
    return f;
  }
  process(_, outs) {
    const o = outs[0][0];
    if (!this.on && this.q.length >= PRE) this.on = true;
    for (let i = 0; i < o.length; i++) {
      if (this.on && (!this.cur || this.at >= this.cur.length)) { this.cur = this.next(); this.at = 0; if (!this.cur) this.on = false; }
      if (this.on) { this.g = Math.min(1, this.g + 1 / FADE); this.last = this.cur[this.at++] / 32768; }
      else this.g = Math.max(0, this.g - 1 / FADE);     // dry: the last sample fades out, no step to zero
      o[i] = this.last * this.g;
    }
    return true;
  }
}
const PRE = 3, MAX = 8, FADE = 80;                       // frames (20 ms), frames, samples (5 ms at 16 kHz)
class Mic extends AudioWorkletProcessor {                // this side's microphone, in 20 ms frames
  constructor() { super(); this.buf = new Int16Array(320); this.n = 0; }
  process(ins) {
    const x = ins[0] && ins[0][0];
    if (x) for (let i = 0; i < x.length; i++) {
      this.buf[this.n++] = Math.max(-32768, Math.min(32767, Math.round(x[i] * 32767)));
      if (this.n === 320) { this.port.postMessage(this.buf.buffer.slice(0)); this.n = 0; }
    }
    return true;
  }
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
        canvas { width: 100%; height: 100%; object-fit: contain; display: block; }
        .msg { position: absolute; color: #ddd; font: 14px var(--paper-font-body1_-_font-family, sans-serif); text-align: center; padding: 0 16px; }
        .bar { display: flex; gap: 8px; align-items: center; padding: 10px 12px; }
        .name { flex: 1; font-weight: 500; }
        button { border: 0; border-radius: 18px; padding: 8px 16px; font: inherit; cursor: pointer; color: #fff; background: var(--success-color, #2e7d32); }
        button.end { background: var(--error-color, #c62828); }
        /* the microphone's switch over the picture, bottom left: muted at the start of every call, so nothing of this
           room reaches the Echo until it is pressed */
        button.mute { position: absolute; left: 12px; bottom: 12px; display: flex; align-items: center; gap: 6px; padding: 8px 14px;
                      background: rgba(0, 0, 0, .6); color: #fff; backdrop-filter: blur(4px); }
        button.mute.on { background: var(--success-color, #2e7d32); }
        button.mute ha-icon { --mdc-icon-size: 20px; }
        button[hidden] { display: none !important; }
      </style>
      <ha-card>
        <div class="v"><canvas width="1280" height="720"></canvas><div class="msg">Drop In to see and talk to ${name}</div>
          <button class="mute" hidden><ha-icon icon="mdi:microphone-off"></ha-icon><span>Unmute</span></button></div>
        <div class="bar"><span class="name">${name}</span>
          <button class="go">Drop In</button><button class="end" hidden>Hang up</button></div>
      </ha-card>`;
    const $ = (s) => this.shadowRoot.querySelector(s);
    this.$ = { canvas: $('canvas'), msg: $('.msg'), go: $('.go'), end: $('.end'), mute: $('.mute') };
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
  live(on) { this.$.go.hidden = on; this.$.end.hidden = !on; this.$.mute.hidden = !on; }

  async start() {
    if (this.ws) return;
    this.live(true); this.setMuted(true); this.say('Calling…');
    try {
      this.ctx = new AudioContext({ sampleRate: 16000 });
      await this.ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([WORKLET], { type: 'application/javascript' })));
      this.player = new AudioWorkletNode(this.ctx, 'echo-local-player');
      this.player.connect(this.ctx.destination);
    } catch (e) { this.stop('No sound here: ' + e.message); return; }
    this.mic = null;
    if (navigator.mediaDevices && navigator.mediaDevices.getUserMedia) {
      try {
        this.stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true } });
        const src = this.ctx.createMediaStreamSource(this.stream);
        this.mic = new AudioWorkletNode(this.ctx, 'echo-local-mic');
        this.mic.port.onmessage = (e) => this.send(e.data);
        src.connect(this.mic);
      } catch (e) { this.listenOnly = 'Your microphone is off for this page (' + e.name + '): listening only'; }
    } else this.listenOnly = 'Listening only: the microphone needs Home Assistant over https or the app';
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
        if (m.state === 'ringing') this.say('Ringing: someone presses the button on the Echo…');
        else if (m.state === 'connected') this.say(this.listenOnly || (this.gotPicture ? '' : 'Connected: no picture (the Echo\'s camera is off or covered)'));
        else if (m.state === 'idle') this.ended = 'The call ended';
      }
      return;
    }
    const b = new Uint8Array(d);
    if (b[0] === 65) this.player.port.postMessage(new Int16Array(d.slice(1)));            // 'A'
    else if (b[0] === 86) this.picture(b[1], new DataView(d).getBigUint64(2, true), b.subarray(10));   // 'V'
  }

  decoderStart() {
    this.config_ = null; this.gotPicture = false;
    if (!('VideoDecoder' in window)) { this.noVideo = 'This browser cannot show the picture (no WebCodecs)'; return; }
    const g = this.$.canvas.getContext('2d');
    this.decoder = new VideoDecoder({
      output: (f) => {
        if (this.$.canvas.width !== f.displayWidth) { this.$.canvas.width = f.displayWidth; this.$.canvas.height = f.displayHeight; }
        g.drawImage(f, 0, 0); f.close();
        if (!this.gotPicture) { this.gotPicture = true; this.say(this.listenOnly || ''); }
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
    if (this.stream) this.stream.getTracks().forEach((t) => t.stop());
    if (this.decoder && this.decoder.state !== 'closed') this.decoder.close();
    if (this.ctx) this.ctx.close();
    this.stream = this.ctx = this.decoder = this.mic = null; this.keyed = false; this.state = ''; this.ended = ''; this.listenOnly = '';
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
