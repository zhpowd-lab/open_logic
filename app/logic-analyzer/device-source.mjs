import { MAX_BYTES, MAX_SAMPLES, WORD_BYTES, CLOCK_HZ, MIN_PERIOD_TICKS, normalizeMetadata, sampleLimit } from './public/capture-core.mjs';

const RESERVED_PORTS = [8098, 8099, 6011]; // 既有采集链路的保留端口。
const REQUEST_TIMEOUT_MS = 3000; // 限制装置故障时的接口等待。
const MAX_METADATA_BYTES = 65536; // 状态响应独立于两兆字节原码预算。
const UINT32_MAX = 0xffffffff; // 固定宽度段序号的上界。
const REQUEST_FIELD_COUNT = 3; // 段、会话、点数或周期、深度、模式。

const captureStates = new Set(['boot', 'idle', 'running', 'full', 'stopped', 'fault', 'off']);

export class DeviceSource {
  constructor(address) {
    this.address = new URL(address);
    if (this.address.protocol !== 'http:' || this.address.username || this.address.password ||
        this.address.pathname !== '/' || this.address.search || this.address.hash ||
        RESERVED_PORTS.includes(Number(this.address.port))) throw new Error('装置地址必须为独立 http 服务根地址');
  }

  async request(route, options = {}, limit = MAX_METADATA_BYTES) {
    const response = await fetch(new URL(route, this.address), {
      ...options, redirect: 'error', signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
    });
    if (!response.ok) {
      const failure = new Error(`装置接口返回 ${response.status}；当前保留段不变`);
      failure.statusCode = response.status === 409 ? 409 : 503;
      throw failure;
    }
    // 同时限制声明长度和实际流，避免错误源突破本项目的有界原码预算。
    const declared = Number(response.headers.get('content-length'));
    if (declared > limit) { await response.body?.cancel(); throw new Error('装置响应超过上限'); }
    const parts = []; let length = 0;
    for await (const block of response.body) {
      length += block.length;
      if (length > limit) throw new Error('装置响应超过上限');
      parts.push(block);
    }
    return Buffer.concat(parts, length);
  }

  async info() {
    const info = JSON.parse((await this.request('/api/capture')).toString('utf8'));
    const metadata = normalizeMetadata(info);
    if (!Number.isInteger(info.captureId) || info.captureId < 0 || info.captureId > UINT32_MAX ||
        typeof info.sourceEpoch !== 'string' || !/^[0-9a-f]{16}$/.test(info.sourceEpoch) ||
        !captureStates.has(info.captureState) || !Number.isInteger(info.sampleCount) ||
        info.sampleCount < 0 || info.sampleCount > sampleLimit(metadata) ||
        info.mode !== 'device' || info.deviceConnected !== true) throw new Error('装置元数据不符合独立采集合同');
    // 缺失的周期、序号、丢样字段由页面显示 unknown；总线错误也不能推断成零丢样。
    return { ...info, demo: false, mode: 'device', deviceConnected: true };
  }

  async samples(query) {
    const capture = query.get('capture'), epoch = query.get('epoch'), count = query.get('count');
    if (!/^[0-9]{1,10}$/.test(capture ?? '') || Number(capture) > UINT32_MAX ||
        !/^[0-9a-f]{16}$/.test(epoch ?? '') || !/^[0-9]{1,7}$/.test(count ?? '') ||
        Number(count) * WORD_BYTES > MAX_BYTES || [...query.keys()].length !== REQUEST_FIELD_COUNT) throw new Error('原码请求需要有效的数据段、会话与点数');
    const raw = await this.request(`/api/samples?${new URLSearchParams({ capture, epoch, count })}`, {}, MAX_BYTES);
    if (raw.length !== Number(count) * WORD_BYTES) throw new Error('装置原码长度与请求点数不一致');
    return raw;
  }

  async arm(query) {
    const period = Number(query.get('period')), count = Number(query.get('count')), pattern = Number(query.get('pattern'));
    if ([...query.keys()].length !== REQUEST_FIELD_COUNT || !query.has('pattern') || !Number.isInteger(period) ||
        period < MIN_PERIOD_TICKS || period > CLOCK_HZ || !Number.isInteger(count) || count < 1 ||
        count > Math.min(MAX_SAMPLES, Math.floor(CLOCK_HZ / period)) || ![0, 1].includes(pattern)) throw new Error('采集周期、深度或源模式无效');
    return this.request(`/api/arm?${new URLSearchParams({ period, count, pattern })}`, {
      method: 'POST', headers: { 'X-Logic16-Arm': '1', 'Content-Length': '0' },
    });
  }
}
