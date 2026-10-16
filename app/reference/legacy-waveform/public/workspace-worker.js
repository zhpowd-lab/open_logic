/* global WorkspaceModel */
// 完整历史仅按用户的细节请求读取一次，在Worker校验80路；不周期拉取大JSON。
importScripts('/workspace/model.js');
let raw = null, meta = null, savedAt = null, sourceFrozen = false, active = null;
self.onmessage = async ({ data: command }) => {
  try {
    if (command.type === 'load') {
      active?.abort(); const controller = new AbortController(); active = controller;
      const timer = setTimeout(() => controller.abort(), 20000);
      try {
        const response = await fetch('/api/monitor-state', { signal: controller.signal, cache: 'no-store' });
        if (!response.ok) throw new Error('原始历史读取失败');
        const envelope = await response.json();
        if (envelope.format !== 'rt_mpu_wave_monitor' || envelope.version !== 1 ||
            typeof envelope.payload !== 'string' || envelope.payload.length > 128 * 1024 * 1024)
          throw new Error('原始历史格式无效');
        const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(envelope.payload));
        const sha = Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, '0')).join('');
        if (sha !== envelope.sha256) throw new Error('原始历史SHA256不一致');
        const state = JSON.parse(envelope.payload);
        const candidate = WorkspaceModel.decodeHistory(state.frozen || state.history);
        if (controller.signal.aborted) return;
        raw = candidate; sourceFrozen = !!state.frozen; meta = sourceFrozen ? state.frozenMeta : state.meta; savedAt = envelope.savedAt;
        self.postMessage({ type: 'loaded', id: command.id, report: raw.report, meta, savedAt, sha256: sha, sourceFrozen });
      } finally { clearTimeout(timer); if (active === controller) active = null; }
    }
    if (command.type === 'load' || command.type === 'view') {
      if (!raw) throw new Error('尚未载入原始历史');
      self.postMessage({ type: 'view', id: command.id, ...WorkspaceModel.historyView(raw, command.selected, command.windowMs, command.range), meta, savedAt, sourceFrozen });
    }
  } catch (error) { self.postMessage({ type: 'error', id: command.id, text: error.name === 'AbortError' ? '原始历史读取已取消或超时' : error.message }); }
};
