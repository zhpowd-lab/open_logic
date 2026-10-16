import assert from 'node:assert/strict';
import http from 'node:http';
import { test } from 'node:test';
import { DeviceSource } from '../device-source.mjs';
import { createLogicServer } from '../server.mjs';
import { makeTestCapture, decodeCapture, cursorAt, findEdge } from '../public/capture-core.mjs';

async function serve(server, action) {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try { await action(`http://127.0.0.1:${server.address().port}`); }
  finally { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); }
}

const metadata = {
  format: 'logic16-le-v1', channels: 16, byteOrder: 'little', sampleCount: 3,
  samplePeriodTicks: 24, clockHz: 24000000, sequenceStart: 0, droppedSamples: 0,
  captureId: 1, sourceEpoch: '0000000000000001', captureState: 'full', mode: 'device', deviceConnected: true,
};

test('装置源校验、事务化原码与显式采集命令经过独立服务', async () => {
  const commands = [], requests = [];
  const upstream = http.createServer((req, res) => {
    requests.push(req.method + ' ' + req.url);
    if (req.url === '/api/capture') res.end(JSON.stringify(metadata));
    else if (req.url.startsWith('/api/samples?')) res.end(Buffer.from([0, 0, 1, 0, 1, 128]));
    else if (req.url.startsWith('/api/arm?')) {
      commands.push({ method: req.method, header: req.headers['x-logic16-arm'] }); res.writeHead(202); res.end('{"accepted":true}');
    } else { res.writeHead(404); res.end(); }
  });
  await serve(upstream, async address => {
    const source = new DeviceSource(address);
    await serve(createLogicServer(makeTestCapture(), { deviceSource: source }), async url => {
      const info = await fetch(url + '/api/capture').then(response => response.json());
      assert.equal(info.captureId, 1); assert.equal(info.deviceConnected, true);
      const binary = await fetch(url + '/api/samples?capture=1&epoch=0000000000000001&count=3').then(response => response.arrayBuffer());
      const capture = decodeCapture(new Uint8Array(binary), info);
      assert.deepEqual([...capture.samples], [0, 1, 0x8001]);
      assert.equal(findEdge(capture.samples, 15, 'rising'), 2);
      assert.equal(cursorAt(capture.samples, capture.metadata, 2).word, 0x8001);
      assert.equal(commands.length, 0);
      assert.equal((await fetch(url + '/api/device/arm?period=24&count=1000000&pattern=0', { method: 'POST' })).status, 405);
      assert.equal((await fetch(url + '/api/device/arm?period=24&count=1000000&pattern=0', { method: 'POST', headers: { 'X-Logic16-Arm': '1' } })).status, 202);
      assert.deepEqual(commands, [{ method: 'POST', header: '1' }]);
      assert.equal(requests.some(request => request.includes('6011')), false);
    });
  });
});

test('只在尾部漏周期时，保留的连续首段仍有确定序号', () => {
  const capture = decodeCapture(Uint8Array.from([1, 0, 2, 0]), {
    samplePeriodTicks: 24, sequenceStart: 10, droppedSamples: 4, droppedAtTail: true,
  });
  assert.equal(cursorAt(capture.samples, capture.metadata, 1).sequence, 11);
});

test('装置故障或缺失字段保留 unknown，不推断零丢样或一微秒', async () => {
  await serve(http.createServer((req, res) => res.end(JSON.stringify({
    ...metadata, samplePeriodTicks: null, sequenceStart: null, droppedSamples: null,
    captureState: 'fault', errorCode: 5,
  }))), async address => {
    const info = await new DeviceSource(address).info();
    const capture = decodeCapture(Uint8Array.from([1, 0, 2, 0, 3, 0]), info);
    assert.equal(capture.metadata.samplePeriodNs, null);
    assert.equal(capture.metadata.sequenceStart, null);
    assert.equal(capture.metadata.droppedSamples, null);
  });
});

test('装置拒绝的旧段返回 409，不混入其它数据段', async () => {
  await serve(http.createServer((req, res) => { res.writeHead(409); res.end('{}'); }), async address => {
    const source = new DeviceSource(address);
    await assert.rejects(source.samples(new URLSearchParams('capture=1&epoch=0000000000000001&count=3')), error => error.statusCode === 409);
  });
});

test('装置异常长度、过深元数据及无效命令均拒绝', async () => {
  await serve(http.createServer((req, res) => {
    if (req.url === '/api/capture') res.end(JSON.stringify({ ...metadata, sampleCount: 1000001 }));
    else res.end(Buffer.from([1, 0]));
  }), async address => {
    const source = new DeviceSource(address);
    await assert.rejects(source.info(), /合同/);
    await assert.rejects(source.samples(new URLSearchParams('capture=1&epoch=0000000000000001&count=3')), /长度/);
    for (const query of ['period=0&count=1&pattern=0', 'period=24&count=1000001&pattern=0',
      'period=240&count=100001&pattern=0', 'period=24&count=10', 'period=24&count=1&pattern=0&pattern=0']) {
      await assert.rejects(source.arm(new URLSearchParams(query)), /无效/);
    }
  });
  for (const address of ['http://127.0.0.1:8098', 'http://127.0.0.1:8099', 'http://127.0.0.1:6011', 'https://127.0.0.1']) {
    assert.throws(() => new DeviceSource(address));
  }
});
