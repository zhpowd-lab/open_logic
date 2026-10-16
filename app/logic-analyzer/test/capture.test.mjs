import test from 'node:test';
import assert from 'node:assert/strict';
import {
  MAX_SAMPLES, normalizeMetadata, decodeCapture, encodeSamples, findEdge,
  centeredWindow, cursorAt, overviewBuckets, makeTestCapture, testPatternWord,
} from '../public/capture-core.mjs';

const meta = { samplePeriodTicks: 24, sequenceStart: 900, droppedSamples: 0 };

test('小端解析覆盖bit0、bit15、偏移视图和编码往返', () => {
  const raw = Uint8Array.of(0xff, 0x01, 0x00, 0x00, 0x80, 0x34, 0x12, 0xff);
  const capture = decodeCapture(raw.subarray(1, 7), meta);
  assert.deepEqual([...capture.samples], [1, 0x8000, 0x1234]);
  assert.deepEqual([...encodeSamples(capture.samples)], [1, 0, 0, 128, 52, 18]);
  assert.equal(cursorAt(capture.samples, capture.metadata, 1).word, 0x8000);
});

test('样本时间、序号和逐点值使用原始索引；阶梯区间向下取整', () => {
  const capture = decodeCapture(Uint8Array.of(1, 0, 2, 0, 4, 0), meta);
  assert.deepEqual(cursorAt(capture.samples, capture.metadata, 1.9), { index: 1, word: 2, timeNs: 1000, sequence: 901 });
  assert.equal(cursorAt(capture.samples, capture.metadata, -1), null);
  assert.equal(cursorAt(capture.samples, capture.metadata, 3), null);
});

test('缺失字段保持unknown，不默认为0或1µs', () => {
  const c = decodeCapture(Uint8Array.of(0x34, 0x12));
  assert.equal(c.metadata.samplePeriodNs, null);
  assert.equal(c.metadata.samplePeriodTicks, null);
  assert.equal(c.metadata.sequenceStart, null);
  assert.equal(c.metadata.droppedSamples, null);
  assert.equal(cursorAt(c.samples, c.metadata, 0).timeNs, null);
  assert.equal(cursorAt(c.samples, c.metadata, 0).sequence, null);
  assert.equal(c.metadata.sampleCount, 1);
});

test('拒绝半样本、空文件、长度冲突、非小端和不合法元数据', () => {
  assert.throws(() => decodeCapture(Uint8Array.of(1)), /完整/);
  assert.throws(() => decodeCapture(new Uint8Array()), /没有/);
  assert.throws(() => decodeCapture(Uint8Array.of(1, 0), { sampleCount: 2 }), /长度/);
  for (const input of [{ byteOrder: 'big' }, { channels: 32 }, { droppedSamples: -1 },
    { sequenceStart: '0' }, { samplePeriodNs: 0 }, { samplePeriodNs: NaN },
    { samplePeriodTicks: 24, samplePeriodNs: 2000 }, { samplePeriodTicks: 24, clockHz: 1000 },
    { samplePeriodTicks: 24_000_001 }, { format: 'old-256-byte-frames' }]) {
    assert.throws(() => normalizeMetadata(input));
  }
  assert.throws(() => decodeCapture(Uint8Array.of(0, 0, 1, 0), { sequenceStart: Number.MAX_SAFE_INTEGER }), /安全整数/);
});

test('24MHz分数周期保留精度；显式纳秒周期独立可用', () => {
  assert.equal(normalizeMetadata({ samplePeriodTicks: 25 }).samplePeriodNs, 25e9 / 24e6);
  assert.equal(normalizeMetadata({ samplePeriodNs: 2500 }).samplePeriodNs, 2500);
  assert.equal(normalizeMetadata({ samplePeriodNs: 1000, samplePeriodTicks: 24 }).samplePeriodNs, 1000);
});

test('三种触发沿、指定通道、搜索起点及分批边界', () => {
  const raw = Uint16Array.of(0, 0, 1, 1, 0, 0x8000, 0x8000, 0);
  assert.equal(findEdge(raw, 0, 'rising'), 2);
  assert.equal(findEdge(raw, 0, 'falling'), 4);
  assert.equal(findEdge(raw, 0, 'either', 3), 4);
  assert.equal(findEdge(raw, 15, 'rising'), 5);
  assert.equal(findEdge(raw, 1, 'either'), -1);
  assert.equal(findEdge(raw, 0, 'rising', 1, 2), -1);
  assert.equal(findEdge(raw, 0, 'rising', 2, 4), 2);
  assert.throws(() => findEdge(raw, 16, 'either'));
});

test('触发始终居中，靠近记录首尾时保留无样本区域', () => {
  for (const index of [1, 500000, 999999]) {
    const window = centeredWindow(index, 512);
    assert.equal((window.start + window.end) / 2, index);
    assert.equal(window.end - window.start, 512);
  }
  assert.equal(centeredWindow(1, 512).start, -255);
});

test('单点窄脉冲与缺口经过概览聚合仍有证据，游标保留原码', () => {
  const samples = new Uint16Array(10000);
  samples.fill(0x8000); samples[4321] = 1;
  const display = overviewBuckets(samples, 0, samples.length, 10);
  const bucket = display.find(b => b.start <= 4321 && b.end > 4321);
  assert.equal(bucket.any & 1, 1);
  assert.equal(bucket.all & 0x8000, 0);
  assert.equal(display.length, 10);
  const raw = cursorAt(samples, normalizeMetadata(meta), 4321);
  assert.equal(raw.word, 1); assert.equal(raw.index, 4321);
  assert.equal(findEdge(samples, 0, 'rising'), 4321);
});

test('固定测试图案脉宽为256/64/16/4µs，唯一人工缺口宽4µs', () => {
  const c = makeTestCapture();
  assert.equal(c.samples.length, MAX_SAMPLES);
  for (const [start, width] of [[128, 256], [512, 64], [768, 16], [896, 4]]) {
    assert.equal(findEdge(c.samples, 0, 'rising', start), start);
    assert.equal(findEdge(c.samples, 0, 'falling', start + 1), start + width);
  }
  for (let i = 500240; i < 500260; i++) assert.equal(c.samples[i] & 1, i >= 500248 && i < 500252 ? 0 : 1);
  assert.equal(findEdge(c.samples, 0, 'falling', 500240), 500248);
  const buckets = overviewBuckets(c.samples, 0, MAX_SAMPLES, 300);
  assert.ok(buckets.length <= 300);
  for (const index of [0, 127, 128, 500247, 500248, 500251, 500252, 999999]) {
    assert.equal(cursorAt(c.samples, c.metadata, index).word, testPatternWord(index));
  }
  assert.equal(findEdge(c.samples, 15, 'either'), -1);
});

test('超过1百万点只保留首段，不循环覆盖且不把排除点数混入丢样数', () => {
  const raw = new Uint16Array(MAX_SAMPLES + 9); raw[0] = 0x1234; raw[MAX_SAMPLES - 1] = 0xabcd; raw[MAX_SAMPLES] = 0xffff;
  const c = decodeCapture(encodeSamples(raw), { samplePeriodNs: 1000, droppedSamples: 7 });
  assert.equal(c.samples.length, MAX_SAMPLES);
  assert.equal(c.samples[0], 0x1234); assert.equal(c.samples.at(-1), 0xabcd);
  assert.equal(c.metadata.depthLimited, true); assert.equal(c.metadata.excludedSamples, 9);
  assert.equal(c.metadata.droppedSamples, 7);
});

test('慢周期同时受到1秒上限约束，且不填补源报告的丢样', () => {
  const c = decodeCapture(encodeSamples(Uint16Array.of(1, 3, 7, 15, 31, 63)), { samplePeriodNs: 200_000_000, droppedSamples: 2 });
  assert.deepEqual([...c.samples], [1, 3, 7, 15, 31]);
  assert.equal(c.metadata.depthLimited, true); assert.equal(c.metadata.excludedSamples, 1);
  assert.equal(c.metadata.droppedSamples, 2);
});

test('有丢样却缺少缺失位置时，后续真实序号不伪造连续', () => {
  const c = decodeCapture(encodeSamples(Uint16Array.of(1, 2)), { samplePeriodNs: 1000, sequenceStart: 700, droppedSamples: 2 });
  assert.equal(cursorAt(c.samples, c.metadata, 0).sequence, 700);
  assert.equal(cursorAt(c.samples, c.metadata, 1).sequence, null);
  assert.equal(cursorAt(c.samples, c.metadata, 1).word, 2);
});
