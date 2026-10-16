const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { DebugDiagnostics } = require('./debug-diagnostics');

// 独立伴随服务不连接 6011，不创建波形 monitor，也不改已有页面的暂停、标定或统计状态。
function createDebugServer({ diagnostics = new DebugDiagnostics(), readFile = fs.readFile } = {}) {
  const files = { '/': ['debug.html', 'text/html'], '/debug': ['debug.html', 'text/html'],
    '/debug.js': ['debug.js', 'text/javascript'], '/debug.css': ['debug.css', 'text/css'] };
  const server = http.createServer(async (request, response) => {
    let pathname;
    try { pathname = new URL(request.url, 'http://localhost').pathname; }
    catch (_) { response.writeHead(400); return response.end(); }
    response.setHeader('Cache-Control', 'no-store');
    if (request.method !== 'GET') { response.writeHead(405, { Allow: 'GET' }); return response.end(); }
    if (pathname === '/favicon.ico') { response.writeHead(204); return response.end(); }
    if (pathname === '/api/debug') {
      const lease = diagnostics.acquire();
      response.once('close', lease.release);
      try {
        const value = await lease.promise;
        if (!response.destroyed) {
          response.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
          response.end(JSON.stringify(value));
        }
      } catch (_) {
        if (!response.destroyed) {
          response.writeHead(500, { 'Content-Type': 'application/json; charset=utf-8' });
          response.end(JSON.stringify({ error: 'DEBUG_REQUEST_FAILED' }));
        }
      } finally { lease.release(); }
      return;
    }
    const file = files[pathname];
    if (!file) { response.writeHead(404); return response.end(); }
    readFile(path.join(__dirname, 'public', file[0]), (error, data) => {
      response.writeHead(error ? 404 : 200, { 'Content-Type': file[1] + '; charset=utf-8' });
      response.end(error ? '' : data);
    });
  });
  server.once('close', () => diagnostics.close());
  return server;
}
if (require.main === module) {
  const port = Number(process.env.RT_MPU_DEBUG_PORT || 8099);
  const diagnostics = new DebugDiagnostics();
  const server = createDebugServer({ diagnostics });
  server.listen(port, '127.0.0.1', () => console.log('独立运行诊断 http://127.0.0.1:' + port + '/'));
  const stop = () => { diagnostics.close(); server.close(); };
  process.once('SIGTERM', stop); process.once('SIGINT', stop);
}
module.exports = { createDebugServer };
