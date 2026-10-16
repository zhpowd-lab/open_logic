export const MAX_SAMPLES = 1_000_000;
export const WORD_BYTES = 2; // 每点固定一个小端 uint16。
export const CLOCK_HZ = 24_000_000; // 原码合同中 tick 的固定时钟。
export const MIN_PERIOD_TICKS = 24; // 装置最快一微秒档。
export const MAX_BYTES = MAX_SAMPLES * WORD_BYTES;
export const MAX_DURATION_NS = 1_000_000_000;

function optionalInteger(value, name, minimum = 0) {
  if (value === undefined || value === null) return null;
  if (!Number.isSafeInteger(value) || value < minimum) throw new Error(`${name} 必须是安全范围内的整数`);
  return value;
}

export function normalizeMetadata(input = {}) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) throw new Error('元数据必须是 JSON 对象');
  if (input.format !== undefined && input.format !== 'logic16-le-v1') throw new Error('不支持的数据格式');
  if (input.channels !== undefined && input.channels !== 16) throw new Error('只支持 16 路');
  if (input.byteOrder !== undefined && input.byteOrder !== 'little') throw new Error('只支持小端 uint16');
  const ticks = optionalInteger(input.samplePeriodTicks, 'samplePeriodTicks', 1);
  const clock = optionalInteger(input.clockHz, 'clockHz', 1);
  if (ticks !== null && clock !== null && clock !== 24_000_000) throw new Error('tick 时钟必须为 24 MHz');
  const ns = input.samplePeriodNs ?? null;
  if (ns !== null && (typeof ns !== 'number' || !Number.isFinite(ns) || ns <= 0 || ns > MAX_DURATION_NS)) {
    throw new Error('samplePeriodNs 必须是 0～1 秒内的正数');
  }
  // samplePeriodTicks 字段在本格式中固定代表24MHz tick，缺失周期绝不推断成1µs。
  const tickNs = ticks === null ? null : ticks * 1e9 / 24_000_000;
  if (tickNs !== null && tickNs > MAX_DURATION_NS) throw new Error('采样周期超过 1 秒');
  if (ns !== null && tickNs !== null && Math.abs(ns - tickNs) > Math.max(ns, tickNs) * 1e-9) {
    throw new Error('纳秒周期和 tick 周期不一致');
  }
  const sequenceStart = optionalInteger(input.sequenceStart, 'sequenceStart');
  return {
    format: 'logic16-le-v1', channels: 16, byteOrder: 'little',
    samplePeriodTicks: ticks, clockHz: ticks === null ? clock : 24_000_000,
    samplePeriodNs: ns ?? tickNs, sequenceStart,
    droppedSamples: optionalInteger(input.droppedSamples, 'droppedSamples'),
    droppedAtTail: input.droppedAtTail === true,
    declaredSampleCount: optionalInteger(input.sampleCount, 'sampleCount'),
    source: typeof input.source === 'string' ? input.source.slice(0, 120) : null,
  };
}

export function sampleLimit(metadata) {
  return Math.min(MAX_SAMPLES, metadata.samplePeriodNs === null ? MAX_SAMPLES :
    Math.floor(MAX_DURATION_NS / metadata.samplePeriodNs + 1e-9));
}

export function decodeCapture(bytes, input = {}, originalSampleCount = bytes.byteLength / 2) {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength % 2) throw new Error('原始文件必须是完整的小端 uint16，不能有半条样本');
  if (!Number.isSafeInteger(originalSampleCount) || originalSampleCount < bytes.byteLength / 2) throw new Error('源样本数无效');
  const metadata = normalizeMetadata(input);
  if (metadata.declaredSampleCount !== null && metadata.declaredSampleCount !== originalSampleCount) {
    throw new Error('元数据 sampleCount 与原始文件长度不一致');
  }
  const count = Math.min(originalSampleCount, sampleLimit(metadata));
  if (!count) throw new Error('数据没有完整样本');
  if (bytes.byteLength < count * 2) throw new Error('原始样本不足');
  if (metadata.sequenceStart !== null && metadata.sequenceStart > Number.MAX_SAFE_INTEGER - (count - 1)) {
    throw new Error('样本序号超过安全整数范围');
  }
  const samples = new Uint16Array(count);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let i = 0; i < count; i++) samples[i] = view.getUint16(i * 2, true);
  return {
    samples,
    metadata: {
      ...metadata, sampleCount: count, sourceSampleCount: originalSampleCount,
      depthLimited: originalSampleCount > count, retainedPrefix: true,
      excludedSamples: originalSampleCount - count,
      depthCapacity: sampleLimit(metadata),
    },
  };
}

export function encodeSamples(samples) {
  const bytes = new Uint8Array(samples.length * 2);
  const view = new DataView(bytes.buffer);
  for (let i = 0; i < samples.length; i++) view.setUint16(i * 2, samples[i], true);
  return bytes;
}

export function findEdge(samples, channel, edge, from = 1, until = samples.length) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) throw new Error('触发通道必须为 D0～D15');
  if (!['rising', 'falling', 'either'].includes(edge)) throw new Error('触发沿无效');
  const mask = 1 << channel;
  for (let i = Math.max(1, Math.ceil(from)); i < Math.min(until, samples.length); i++) {
    const before = !!(samples[i - 1] & mask), after = !!(samples[i] & mask);
    if (before !== after && (edge === 'either' || (edge === 'rising' ? after : before))) return i;
  }
  return -1;
}

export function centeredWindow(index, span) {
  // 首尾触发也保持居中；超出记录的区域交给绘制层明确显示无样本。
  const width = Math.max(2, Math.round(span));
  return { start: index - width / 2, end: index + width / 2 };
}

export function cursorAt(samples, metadata, samplePosition) {
  if (!Number.isFinite(samplePosition)) return null;
  const index = Math.floor(samplePosition);
  if (index < 0 || index >= samples.length) return null;
  return {
    index, word: samples[index],
    timeNs: metadata.samplePeriodNs === null ? null : index * metadata.samplePeriodNs,
    // 有丢样但没有缺失位置时，不能把保留数组的索引冒充后续装置序号。
    sequence: metadata.sequenceStart === null || (metadata.droppedSamples > 0 && !metadata.droppedAtTail && index > 0) ? null : metadata.sequenceStart + index,
  };
}

export function overviewBuckets(samples, start, end, pixels) {
  const first = Math.max(0, Math.floor(start)), last = Math.min(samples.length, Math.ceil(end));
  if (first >= last) return [];
  const stride = Math.max(1, Math.ceil((last - first) / Math.max(1, Math.floor(pixels))));
  const buckets = [];
  // 桶内AND/OR保留单样点脉冲和缺口；桶不是一条伪造的原始样本。
  for (let i = first; i < last; i += stride) {
    const stop = Math.min(last, i + stride);
    let all = 0xffff, any = 0;
    for (let j = i; j < stop; j++) { all &= samples[j]; any |= samples[j]; }
    buckets.push({ start: i, end: stop, first: samples[i], last: samples[stop - 1], all, any });
  }
  return buckets;
}

export function testPatternWord(index) {
  const phase = index % 1024;
  let word = (phase >= 128 && phase < 384) || (phase >= 512 && phase < 576) ||
    (phase >= 768 && phase < 784) || (phase >= 896 && phase < 900) ? 1 : 0;
  // 唯一人工缺口在D0高电平内，宽4样点；它不是传输丢样。
  if (index >= 500248 && index < 500252) word = 0;
  for (let bit = 1; bit < 15; bit++) {
    if (Math.floor(index / (2 ** (bit + 1))) % 2) word |= 1 << bit;
  }
  return word;
}

export function makeTestCapture() {
  const samples = Uint16Array.from({ length: MAX_SAMPLES }, (_, i) => testPatternWord(i));
  return decodeCapture(encodeSamples(samples), {
    format: 'logic16-le-v1', channels: 16, byteOrder: 'little',
    samplePeriodTicks: 24, clockHz: 24_000_000, sequenceStart: 0, droppedSamples: 0,
    sampleCount: MAX_SAMPLES, source: '固定测试图案 · 非装置采集',
  });
}
