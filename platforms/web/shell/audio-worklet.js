// Plays the game's audio from the ring buffer its mixer thread fills in shared wasm memory
// (src/dusk/web/audio.cpp). Header, as 32-bit words: [0] frames read, [1] frames written (both
// free-running), [2] playing, [3] underrun frames. Samples are interleaved float32; the ring's
// capacity in frames is a power of two.
class DuskAudioProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { memory, header, data, frames, channels } = options.processorOptions;
    Object.assign(this, { memory, header, data, frames, channels, buffer: null });
  }

  process(inputs, outputs) {
    if (this.buffer !== this.memory.buffer) {
      // Growing the memory replaces its buffer.
      this.buffer = this.memory.buffer;
      this.ring = new Uint32Array(this.buffer, this.header, 4);
      this.wake = new Int32Array(this.buffer, this.header, 1);
      this.samples = new Float32Array(this.buffer, this.data, this.frames * this.channels);
    }
    const out = outputs[0];
    const n = out[0].length;
    const read = Atomics.load(this.ring, 0);
    const count = Math.min(n, (Atomics.load(this.ring, 1) - read) >>> 0);
    for (let i = 0; i < count; ++i) {
      const base = ((read + i) % this.frames) * this.channels;
      for (let c = 0; c < out.length; ++c) out[c][i] = this.samples[base + Math.min(c, this.channels - 1)];
    }
    for (let c = 0; c < out.length; ++c) out[c].fill(0, count);
    if (count < n) this.ring[3] += n - count;
    Atomics.store(this.ring, 0, (read + count) >>> 0);
    // Wakes the mixer thread waiting for room.
    Atomics.notify(this.wake, 0);
    return true;
  }
}

registerProcessor('dusk-audio', DuskAudioProcessor);
