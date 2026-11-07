/* Transport-only responder: deliberately no GPU emulation or rendering claim. */
import {parentPort, workerData} from 'node:worker_threads';
import {responseViews, publishResponse} from '../../public/graphics-protocol.js';
let views, byte = 0, presents = 0;
const mode = workerData?.mode;
function handle(message) {
  if (message.command === 'init') {
    views = responseViews(message.args.response);
    if (mode === 'timeout') return;
    publishResponse(views, mode === 'serial' ? message.serial + 1 : message.serial, {backend: 'transport-test-only', profile:message.args.profile === true}); return;
  }
  if (message.command === 'uploadVertices') { byte = new Uint8Array(message.args.bytes)[0]; publishResponse(views, message.serial, 1); }
  else if (message.command === 'readPixels') publishResponse(views, message.serial, {width: 1, height: 1, rgba: new Uint8Array([byte, 20, 30, 255])});
  else if (message.command === 'present' && mode === 'profile') publishResponse(views,message.serial,{serial:++presents,acknowledged:true,metrics:{fixtureOnly:true}});
  else if (message.command === 'present') publishResponse(views, message.serial, {error: 'intentional transport error'}, true);
  else publishResponse(views, message.serial, null);
}
parentPort.on('message', message => {
  if (message.type === 'connect') { message.port.on('message', handle); parentPort.postMessage('connected'); }
  else handle(message);
});
