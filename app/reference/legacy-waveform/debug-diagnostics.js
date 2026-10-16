const { execFile } = require('node:child_process');
const path = require('node:path');

function unavailable(error, device) {
  return { version: 1, readAt: new Date().toISOString(), device,
    e907: { available: false, error }, linux: { available: false, error } };
}

// 诊断只有浏览器请求的持有者存在时执行；多个页面共用一次有界进程，不启动定时探测。
class DebugDiagnostics {
  constructor({ device = '192.168.64.240', execute = execFile,
    python = process.env.RT_MPU_PYTHON || (process.platform === 'win32' ? 'py' : 'python3') } = {}) {
    this.device = device; this.execute = execute; this.python = python;
    this.inflight = null; this.closed = false;
  }
  acquire() {
    if (this.closed) return { promise: Promise.resolve(unavailable('SERVICE_CLOSED', this.device)), release() {} };
    let state = this.inflight;
    if (!state) {
      let resolve;
      state = { users: 0, child: null, done: false,
        promise: new Promise(done => { resolve = done; }) };
      state.finish = value => {
        if (state.done) return;
        state.done = true;
        resolve(value);
      };
      this.inflight = state;
      try {
        state.child = this.execute(this.python, ['-X', 'utf8', path.join(__dirname, '../tools/wave_debug.py'),
          '--ip', this.device], { timeout: 12000, windowsHide: true, maxBuffer: 65536 }, (error, stdout) => {
          // execFile 回调确认进程和双流已结束；kill 请求本身不能释放唯一进程槽。
          if (this.inflight === state) this.inflight = null;
          if (state.done) return;
          if (error) return state.finish(unavailable(error.killed ? 'PROBE_TIMEOUT' : 'PROBE_FAILED', this.device));
          try {
            const value = JSON.parse(stdout);
            if (value.version !== 1 || value.device !== this.device || typeof value.readAt !== 'string' ||
              !Number.isFinite(Date.parse(value.readAt)) || !value.e907 || !value.linux ||
              typeof value.e907.available !== 'boolean' || typeof value.linux.available !== 'boolean') {
              throw new Error('Invalid debug response');
            }
            state.finish(value);
          } catch (_) { state.finish(unavailable('BAD_REPLY', this.device)); }
        });
      } catch (_) {
        if (this.inflight === state) this.inflight = null;
        state.finish(unavailable('PROBE_FAILED', this.device));
      }
    }
    // 同步失败回调也可以安全取得已经完成的请求结果。
    if (!state) return { promise: Promise.resolve(unavailable('PROBE_FAILED', this.device)), release() {} };
    ++state.users;
    let released = false;
    return { promise: state.promise, release: () => {
      if (released) return;
      released = true; --state.users;
      if (!state.users && !state.done) {
        state.child?.kill();
        state.finish(unavailable('REQUEST_CANCELLED', this.device));
      }
    } };
  }
  close() {
    this.closed = true;
    const state = this.inflight;
    if (state && !state.done) {
      state.child?.kill(); state.finish(unavailable('SERVICE_CLOSED', this.device));
    }
  }
}
module.exports = { DebugDiagnostics };
