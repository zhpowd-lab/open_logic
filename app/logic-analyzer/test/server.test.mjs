import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createLogicServer, readFileCapture } from '../server.mjs';
import { decodeCapture, encodeSamples } from '../public/capture-core.mjs';

test('独立HTTP服务的元数据/二进制合同、静态资源与只读接口', async t => {
  const capture = decodeCapture(Uint8Array.of(1, 0, 0, 128, 0x34, 0x12), { samplePeriodTicks: 24, sequenceStart: 42, droppedSamples: 0 });
  const server = createLogicServer(capture, { demo: false });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => { server.closeAllConnections(); server.close(resolve); }));
  const base = `http://127.0.0.1:${server.address().port}`;
  const metadata = await (await fetch(`${base}/api/capture`)).json();
  assert.equal(metadata.deviceConnected, false); assert.equal(metadata.sequenceStart, 42);
  assert.equal(metadata.sampleCount, 3); assert.equal(metadata.samplePeriodNs, 1000);
  const response = await fetch(`${base}/api/samples`);
  const bytes = new Uint8Array(await response.arrayBuffer());
  assert.deepEqual([...decodeCapture(bytes, metadata).samples], [1, 0x8000, 0x1234]);
  for (const asset of ['/', '/app.mjs', '/capture-core.mjs', '/style.css', '/vendor/lucide.min.js']) assert.equal((await fetch(base + asset)).status, 200);
  assert.equal((await fetch(`${base}/api/stop`, { method: 'POST' })).status, 405);
  assert.equal((await fetch(`${base}/api/samples`, { method: 'HEAD' })).headers.get('content-length'), '6');
  assert.equal((await fetch(`${base}/server.mjs`)).status, 404);
});

test('本地文件源读取，缺元数据保留unknown，坏元数据拒绝', t => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'open-logic-test-'));
  t.after(() => fs.rmSync(dir, { recursive: true }));
  const file = path.join(dir, 'sample.u16'), json = path.join(dir, 'sample.json');
  fs.writeFileSync(file, encodeSamples(Uint16Array.of(0x1234, 0x8001)));
  assert.equal(readFileCapture(file).metadata.droppedSamples, null);
  fs.writeFileSync(json, JSON.stringify({ samplePeriodNs: 1000, sequenceStart: 7, droppedSamples: 3, sampleCount: 2 }));
  const c = readFileCapture(file, json);
  assert.deepEqual([...c.samples], [0x1234, 0x8001]); assert.equal(c.metadata.sequenceStart, 7);
  fs.writeFileSync(json, '{invalid'); assert.throws(() => readFileCapture(file, json));
  fs.writeFileSync(file, Buffer.from([1])); assert.throws(() => readFileCapture(file), /偶数/);
});
