import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { MAX_BYTES, decodeCapture, encodeSamples, makeTestCapture } from './public/capture-core.mjs';
import { DeviceSource } from './device-source.mjs';

const directory = path.dirname(fileURLToPath(import.meta.url));
const assets = new Map([
  ['/', ['index.html', 'text/html; charset=utf-8']],
  ['/app.mjs', ['app.mjs', 'text/javascript; charset=utf-8']],
  ['/capture-core.mjs', ['capture-core.mjs', 'text/javascript; charset=utf-8']],
  ['/style.css', ['style.css', 'text/css; charset=utf-8']],
  ['/vendor/lucide.min.js', ['vendor/lucide.min.js', 'text/javascript; charset=utf-8']],
]);

export function readFileCapture(file, metaFile) {
  const size = fs.statSync(file).size;
  if (!size || size % 2) throw new Error('原始文件长度必须为非零偶数');
  const input = metaFile ? JSON.parse(fs.readFileSync(metaFile, 'utf8').replace(/^\uFEFF/, '')) : {};
  const bytes = Buffer.alloc(Math.min(size, MAX_BYTES));
  const fd = fs.openSync(file, 'r');
  try {
    let offset = 0;
    while (offset < bytes.length) {
      const read = fs.readSync(fd, bytes, offset, bytes.length - offset, offset);
      if (!read) throw new Error('读取时源文件发生截断');
      offset += read;
    }
  } finally { fs.closeSync(fd); }
  return decodeCapture(bytes, input, size / 2);
}

export function createLogicServer(capture = makeTestCapture(), { demo = true, deviceSource = null } = {}) {
  const binary = Buffer.from(encodeSamples(capture.samples));
  const metadata = { ...capture.metadata, demo, mode: 'local-replay', deviceConnected: false };
  return http.createServer(async (req, res) => {
    const headers = { 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' };
    const url = new URL(req.url, 'http://localhost');
    const pathname = url.pathname;
    if (deviceSource && pathname.startsWith('/api/')) {
      try {
        let body, type = 'application/json; charset=utf-8', status = 200;
        if ((req.method === 'GET' || req.method === 'HEAD') && pathname === '/api/capture') body = JSON.stringify(await deviceSource.info());
        else if ((req.method === 'GET' || req.method === 'HEAD') && pathname === '/api/samples') {
          body = await deviceSource.samples(url.searchParams); type = 'application/octet-stream';
        } else if (req.method === 'POST' && pathname === '/api/device/arm' && req.headers['x-logic16-arm'] === '1' &&
                   (!req.headers.origin || req.headers.origin === `http://${req.headers.host}`) &&
                   !req.headers['transfer-encoding'] && (!req.headers['content-length'] || req.headers['content-length'] === '0')) {
          body = await deviceSource.arm(url.searchParams); status = 202;
        } else {
          res.writeHead(405, { ...headers, Allow: 'GET, HEAD' }); res.end(); return;
        }
        res.writeHead(status, { ...headers, 'Content-Type': type, 'Content-Length': Buffer.byteLength(body) });
        res.end(req.method === 'HEAD' ? undefined : body);
      } catch (error) {
        const body = JSON.stringify({ error: error.message });
        res.writeHead(error.statusCode ?? 503, { ...headers, 'Content-Type': 'application/json; charset=utf-8' }); res.end(body);
      }
      return;
    }
    if (req.method !== 'GET' && req.method !== 'HEAD') {
      res.writeHead(405, { ...headers, Allow: 'GET, HEAD' }); res.end(); return;
    }
    let body, type;
    if (pathname === '/api/capture') { body = JSON.stringify(metadata); type = 'application/json; charset=utf-8'; }
    else if (pathname === '/api/samples') { body = binary; type = 'application/octet-stream'; }
    else if (assets.has(pathname)) {
      const [file, mime] = assets.get(pathname);
      body = fs.readFileSync(path.join(directory, 'public', file)); type = mime;
    } else { res.writeHead(404, headers); res.end('Not found'); return; }
    res.writeHead(200, { ...headers, 'Content-Type': type, 'Content-Length': Buffer.byteLength(body) });
    res.end(req.method === 'HEAD' ? undefined : body);
  });
}

function main() {
  const args = process.argv.slice(2);
  const options = {};
  for (let i = 0; i < args.length; i += 2) {
    if (!['--port', '--file', '--meta', '--device'].includes(args[i]) || !args[i + 1]) throw new Error('用法：node server.mjs [--port 8102] [--file capture.u16] [--meta capture.json] [--device http://装置:8102]');
    options[args[i].slice(2)] = args[i + 1];
  }
  const port = Number(options.port ?? 8102);
  if (!Number.isInteger(port) || port < 1024 || port > 65535 || [8098, 8099, 6011].includes(port)) throw new Error('端口无效或属于保留服务');
  if (options.meta && !options.file) throw new Error('--meta 需要同时指定 --file');
  if (options.device && options.file) throw new Error('装置源与文件源只能选择一个');
  const server = createLogicServer(options.file ? readFileCapture(options.file, options.meta) : makeTestCapture(), {
    demo: !options.file, deviceSource: options.device ? new DeviceSource(options.device) : null,
  });
  server.on('error', error => { console.error(`逻辑分析仪启动失败：${error.message}`); process.exitCode = 1; });
  server.listen(port, '127.0.0.1', () => console.log(`逻辑分析仪：http://127.0.0.1:${port}/ · ${options.device ? '装置独立采集源' : options.file ? '文件回放' : '固定测试图案'}`));
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { main(); } catch (error) { console.error(error.message); process.exitCode = 1; }
}
