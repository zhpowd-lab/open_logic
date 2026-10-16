import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { testPatternWord, decodeCapture, findEdge, cursorAt, overviewBuckets } from '../../app/logic-analyzer/public/capture-core.mjs';

const [name, periodText = '24', countText = '1000000', patternText = '0'] = process.argv.slice(2);
if (!/^[a-z0-9-]+$/.test(name ?? '')) throw new Error('需要新的、明确命名的证据目录');
const root = path.join(path.dirname(fileURLToPath(import.meta.url)), 'device-captures', name);
if (fs.existsSync(root)) throw new Error('证据目录已存在，拒绝覆盖或重复采集');
fs.mkdirSync(root, { recursive: true });
const period = Number(periodText), count = Number(countText), pattern = Number(patternText);
const address = 'http://192.168.2.100:8102';
async function info() {
  const response = await fetch(address + '/api/capture', { signal: AbortSignal.timeout(4000) });
  if (!response.ok) throw new Error('状态读取失败');
  return response.json();
}
async function raw(metadata) {
  const query = new URLSearchParams({ capture: metadata.captureId, epoch: metadata.sourceEpoch, count: metadata.sampleCount });
  const response = await fetch(address + '/api/samples?' + query, { signal: AbortSignal.timeout(4000) });
  if (!response.ok) throw new Error('原始数据读取失败');
  const result = Buffer.from(await response.arrayBuffer());
  if (result.length !== metadata.sampleCount * 2) throw new Error('原码长度不一致');
  return result;
}
const before = await info();
if (before.captureState === 'running' || before.pendingCommand) throw new Error('装置已有未完成采集，拒绝替换');
const request = { period, count, pattern };
fs.writeFileSync(path.join(root, 'request.json'), JSON.stringify(request, null, 2));
const response = await fetch(address + '/api/arm?' + new URLSearchParams(request), {
  method: 'POST', headers: { 'X-Logic16-Arm': '1' }, signal: AbortSignal.timeout(4000),
});
if (response.status !== 202) throw new Error('采集请求未获接受：' + response.status);
const progress = [], deadline = Date.now() + 6000;
let current, prefix = Buffer.alloc(0), previousCount = 0;
do {
  current = await info();
  if (current.captureId !== before.captureId || current.sourceEpoch !== before.sourceEpoch) {
    progress.push({ count: current.sampleCount, state: current.captureState });
    if (current.sampleCount < previousCount) throw new Error('提交样本数倒退');
    previousCount = current.sampleCount;
    const next = await raw(current);
    if (!next.subarray(0, prefix.length).equals(prefix)) throw new Error('已提交的首段发生变化');
    prefix = next;
    if (current.captureState !== 'running') break;
  }
  await new Promise(resolve => setTimeout(resolve, 100));
} while (Date.now() < deadline);
if (current.captureId === before.captureId && current.sourceEpoch === before.sourceEpoch) throw new Error('采集命令结果未确认，没有重复发送');
fs.writeFileSync(path.join(root, 'capture.json'), JSON.stringify(current, null, 2));
fs.writeFileSync(path.join(root, 'capture.u16'), prefix);
fs.writeFileSync(path.join(root, 'progress.json'), JSON.stringify(progress, null, 2));
const result = {
  periodTicks: current.samplePeriodTicks, requestedCount: count, retainedCount: current.sampleCount,
  state: current.captureState, error: current.errorCode, dropped: current.droppedSamples,
  bytes: prefix.length, minIntervalTicks: current.minIntervalTicks, maxIntervalTicks: current.maxIntervalTicks,
  maxReadTicks: current.maxReadTicks, maxLateTicks: current.maxLateTicks, pattern,
  progressSnapshots: progress.length, immutablePrefix: true,
  primingReads: current.primingReads, primingMaxTicks: current.primingMaxTicks,
};
if (pattern && current.sampleCount) {
  for (let i = 0; i < current.sampleCount; i++) if (prefix.readUInt16LE(i * 2) !== testPatternWord(i)) throw new Error('CPU3 图案与规定值不一致，索引 ' + i);
  result.allPatternWordsMatch = true;
  if (current.sampleCount > 500252) {
    const capture = decodeCapture(prefix, current);
    result.notchTriggerIndex = findEdge(capture.samples, 0, 'falling', 500240);
    overviewBuckets(capture.samples, 0, capture.samples.length, 200);
    result.cursorWordAfterOverview = cursorAt(capture.samples, capture.metadata, 500248).word;
  }
} else if (current.sampleCount) {
  result.firstWord = '0x' + prefix.readUInt16LE(0).toString(16).padStart(4, '0');
  result.lastWord = '0x' + prefix.readUInt16LE(prefix.length - 2).toString(16).padStart(4, '0');
}
fs.writeFileSync(path.join(root, 'result.json'), JSON.stringify(result, null, 2));
console.log(JSON.stringify(result));
if (current.captureState !== 'full' || current.sampleCount !== count || current.errorCode || current.droppedSamples) process.exitCode = 1;
