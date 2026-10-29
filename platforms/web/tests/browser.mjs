// Shared pieces of the browser tests: Chromium with WebGPU on SwiftShader, and an offscreen canvas
// context for it.
//
// Headless Chromium cannot allocate a WebGPU canvas swap chain on SwiftShader (the device is lost
// on the first present), so installOffscreenCanvasContext replaces the page's GPUCanvasContext with
// one that hands out ordinary textures on the same device. The game's whole render and present path
// still runs; only the browser's final compositing is skipped. Install it with
// page.addInitScript(installOffscreenCanvasContext) before the page's scripts run.
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);

export function launchChromium() {
  const { chromium } = require('playwright');
  return chromium.launch({
    args: ['--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-webgpu-adapter=swiftshader'],
  });
}

export function installOffscreenCanvasContext() {
  const configs = new WeakMap();
  GPUCanvasContext.prototype.configure = function (config) {
    configs.set(this, { config, texture: null });
  };
  GPUCanvasContext.prototype.unconfigure = function () {
    configs.delete(this);
  };
  GPUCanvasContext.prototype.getConfiguration = function () {
    return configs.get(this)?.config ?? null;
  };
  GPUCanvasContext.prototype.getCurrentTexture = function () {
    const state = configs.get(this);
    if (!state) throw new DOMException('context is not configured', 'InvalidStateError');
    const { width, height } = this.canvas;
    if (!state.texture || state.texture.width !== width || state.texture.height !== height) {
      state.texture?.destroy();
      state.texture = state.config.device.createTexture({
        size: [width, height],
        format: state.config.format,
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING |
          GPUTextureUsage.COPY_DST,
      });
    }
    globalThis.duskTestFrames = (globalThis.duskTestFrames ?? 0) + 1;
    globalThis.duskTestLast = { texture: state.texture, device: state.config.device };
    return state.texture;
  };
  // How many distinct colours a sample of the last presented frame has.
  globalThis.duskTestSampleFrame = async () => {
    const last = globalThis.duskTestLast;
    if (!last) return null;
    const { texture, device } = last;
    const bytesPerRow = Math.ceil((texture.width * 4) / 256) * 256;
    const buffer = device.createBuffer({ size: bytesPerRow * texture.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const encoder = device.createCommandEncoder();
    encoder.copyTextureToBuffer({ texture }, { buffer, bytesPerRow }, [texture.width, texture.height]);
    device.queue.submit([encoder.finish()]);
    await buffer.mapAsync(GPUMapMode.READ);
    const bytes = new Uint8Array(buffer.getMappedRange());
    const colours = new Set();
    for (let y = 0; y < texture.height; y += 7) {
      for (let x = 0; x < texture.width; x += 7) {
        const i = y * bytesPerRow + x * 4;
        colours.add((bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2]);
      }
    }
    buffer.unmap();
    buffer.destroy();
    return { colours: colours.size, width: texture.width, height: texture.height };
  };
  // The last presented frame as a PNG data URL.
  globalThis.duskTestFramePng = async () => {
    const last = globalThis.duskTestLast;
    if (!last) return null;
    const { texture, device } = last;
    const bytesPerRow = Math.ceil((texture.width * 4) / 256) * 256;
    const buffer = device.createBuffer({ size: bytesPerRow * texture.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const encoder = device.createCommandEncoder();
    encoder.copyTextureToBuffer({ texture }, { buffer, bytesPerRow }, [texture.width, texture.height]);
    device.queue.submit([encoder.finish()]);
    await buffer.mapAsync(GPUMapMode.READ);
    const src = new Uint8Array(buffer.getMappedRange());
    const image = new ImageData(texture.width, texture.height);
    const bgra = texture.format.startsWith('bgra');
    for (let y = 0; y < texture.height; ++y) {
      for (let x = 0; x < texture.width; ++x) {
        const i = y * bytesPerRow + x * 4;
        const o = (y * texture.width + x) * 4;
        image.data[o] = src[i + (bgra ? 2 : 0)];
        image.data[o + 1] = src[i + 1];
        image.data[o + 2] = src[i + (bgra ? 0 : 2)];
        image.data[o + 3] = 255;
      }
    }
    buffer.unmap();
    buffer.destroy();
    const canvas = new OffscreenCanvas(texture.width, texture.height);
    canvas.getContext('2d').putImageData(image, 0, 0);
    const blob = await canvas.convertToBlob({ type: 'image/png' });
    return await new Promise((resolve) => {
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result);
      reader.readAsDataURL(blob);
    });
  };
}
