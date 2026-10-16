import fs from 'node:fs';

const SOURCE_URL = 'http://127.0.0.1:8102/api/capture';
const OBSERVATION_DURATION_MS = 20000;
const POLL_INTERVAL_MS = 200;
const REQUEST_TIMEOUT_MS = 3000;
const CHANNEL_COUNT = 16;
const WORD_MAX = 0xffff;
const output = process.argv[2];

if (!output || fs.existsSync(output)) throw new Error('请指定尚不存在的观测结果文件');

const records = [];
const words = new Set();
const began = performance.now();
const deadline = began + OBSERVATION_DURATION_MS;
let sourceEpoch = null;
let changedBits = 0;
let failure = null;

// 只记录装置公开的当前值；不ARM，不访问样本区，不保存会话标识。
while (performance.now() < deadline) {
  try {
    const response = await fetch(SOURCE_URL, { signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS) });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const info = await response.json();
    if (!info.deviceConnected || !info.liveMonitorSupported) throw new Error('当前不是装置实时监视源');
    if (sourceEpoch !== null && info.sourceEpoch !== sourceEpoch) throw new Error('观测期间采集核会话变化，已停止记录');
    sourceEpoch = info.sourceEpoch;
    const word = info.liveWord;
    if (word !== null && (!Number.isInteger(word) || word < 0 || word > WORD_MAX)) throw new Error('当前原码字段无效');
    records.push({
      elapsedMs: Math.round(performance.now() - began),
      liveWord: word,
      liveReadSequence: info.liveReadSequence ?? null,
      liveErrorCode: info.liveErrorCode ?? null,
      liveMonitorEnabled: info.liveMonitorEnabled ?? null,
      captureState: info.captureState,
      sampleCount: info.sampleCount,
    });
    if (word !== null) {
      if (words.size) changedBits |= word ^ words.values().next().value;
      words.add(word);
    }
  } catch (error) {
    failure = error.message;
    break;
  }
  await new Promise(resolve => setTimeout(resolve, Math.min(POLL_INTERVAL_MS, Math.max(0, deadline - performance.now()))));
}

const result = {
  source: SOURCE_URL,
  finishedAt: new Date().toISOString(),
  durationMs: Math.round(performance.now() - began),
  pollIntervalMs: POLL_INTERVAL_MS,
  recordCount: records.length,
  uniqueWords: [...words].map(word => `0x${word.toString(16).padStart(4, '0')}`),
  changedChannels: Array.from({ length: CHANNEL_COUNT }, (_, channel) => channel).filter(channel => (changedBits >> channel) & 1),
  sequenceProgress: records.some(record => record.liveReadSequence !== records[0].liveReadSequence),
  failure,
  records,
};
fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n', { flag: 'wx' });
const { records: omitted, ...summary } = result;
console.log(JSON.stringify(summary, null, 2));
if (failure) process.exitCode = 1;
