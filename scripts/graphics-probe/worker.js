import {MoviesGraphicsClient} from './graphics-client.js';
function vertexData(color, z = .5, uv = false, narrow = false) {
  const stride = uv ? 24 : 16, bytes = new ArrayBuffer(stride * 3), view = new DataView(bytes);
  const w = narrow ? .2 : .8;
  [[-w, -.8, z], [w, -.8, z], [0, .8, z]].forEach((p, i) => {
    p.forEach((x, j) => view.setFloat32(i * stride + j * 4, x, true));
    view.setUint32(i * stride + 12, color, true);
    if (uv) { view.setFloat32(i * stride + 16, .5, true); view.setFloat32(i * stride + 20, .5, true); }
  });
  return bytes;
}
function pixel(image, x, y) { return Array.from(image.rgba.slice((y * image.width + x) * 4, (y * image.width + x) * 4 + 4)); }
function expectPixel(image, x, y, expected, label) {
  const actual = pixel(image, x, y);
  if (actual.some((v, i) => Math.abs(v - expected[i]) > 1)) throw new Error(`${label}: ${actual} expected ${expected}`);
  return {label, x, y, rgba: actual};
}
self.onmessage = async ({data}) => {
  if (data.type !== 'run') return;
  let graphics;
  try {
    const width = 190, height = 128;
    graphics = new MoviesGraphicsClient({width, height, presentationAck: data.ack, presentationPort: data.presentationPort, commandPort: data.commandPort, responseBuffer: data.responseBuffer});
    const checks = [{label: 'Dedicated GPU worker initialized while guest worker blocked', passed: true}], cx = Math.floor(width / 2), cy = Math.floor(height / 2);
    const indices = graphics.uploadIndices(new Uint16Array([0, 1, 2]));
    const red = graphics.uploadVertices(vertexData(0xffff0000));
    graphics.clear({color: 0xff112233});
    checks.push(expectPixel(graphics.readPixels(), 0, 0, [17, 34, 51, 255], 'clear/readback/BGRA conversion/row padding'));
    graphics.batch([{command: 'clear', args: {}}, {command: 'drawIndexed', args: {vertices: red, indices}}]);
    let pixels = graphics.readPixels();
    checks.push(expectPixel(pixels, cx, cy, [255, 0, 0, 255], 'indexed XYZ diffuse draw'));
    checks.push(expectPixel(pixels, 0, 0, [0, 0, 0, 255], 'triangle coverage'));
    const indices32 = graphics.uploadIndices(new Uint32Array([1, 2, 3]), 'uint32');
    graphics.clear(); graphics.drawIndexed({vertices: red, indices: indices32, baseVertex: -1});
    checks.push(expectPixel(graphics.readPixels(), cx, cy, [255, 0, 0, 255], 'uint32 indices with signed base vertex'));
    const narrow = graphics.uploadVertices(vertexData(0xffff0000, .5, false, true));
    const world = [1,0,0,0, 0,1,0,0, 0,0,1,0, .6,0,0,1];
    const view = [1,0,0,0, 0,1,0,0, 0,0,1,0, -.2,0,0,1];
    const projection = [.5,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
    graphics.clear(); graphics.drawIndexed({vertices: narrow, indices, world, view, projection});
    pixels = graphics.readPixels();
    checks.push(expectPixel(pixels, Math.floor(width * .6), cy, [255, 0, 0, 255], 'D3D row-major world/view/projection'));
    checks.push(expectPixel(pixels, cx, cy, [0, 0, 0, 255], 'transformed triangle leaves original center'));
    if (graphics.capabilities.depthFormat) {
      const near = graphics.uploadVertices(vertexData(0xffff0000, .2));
      const far = graphics.uploadVertices(vertexData(0xff00ff00, .8));
      graphics.clear(); graphics.drawIndexed({vertices: near, indices, depthTest: true});
      graphics.drawIndexed({vertices: far, indices, depthTest: true});
      checks.push(expectPixel(graphics.readPixels(), cx, cy, [255, 0, 0, 255], 'actual depth16 occlusion'));
    }
    const textured = graphics.uploadVertices(vertexData(0xffffffff, .5, true), 'XYZ_DIFFUSE_UV');
    const texture = graphics.uploadTextureRGBA(1, 1, new Uint8Array([0, 255, 0, 255]));
    graphics.clear({color: 0xff112233}); graphics.drawIndexed({vertices: textured, indices, texture});
    checks.push(expectPixel(graphics.readPixels(), cx, cy, [0, 255, 0, 255], 'UV texture modulation'));
    graphics.validate();
    const presentation = graphics.present();
    graphics.validate();
    self.postMessage({type: 'result', result: {status: 'passed', label: 'Graphics diagnostic only, not the game', capabilities: graphics.capabilities, checks, presentation, transport: 'Guest worker synchronously Atomics.wait; dedicated GPU worker completes async readback; direct MessagePort bitmap presentation to page'}});
  } catch (error) { self.postMessage({type: 'error', message: String(error?.stack || error)}); }
  finally { graphics?.dispose(); }
};
