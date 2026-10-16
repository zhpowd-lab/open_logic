import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { MAX_SAMPLES, encodeSamples } from '../../app/logic-analyzer/public/capture-core.mjs';

const directory = path.join(path.dirname(fileURLToPath(import.meta.url)), 'fixtures');
fs.mkdirSync(directory, { recursive: true });
fs.writeFileSync(path.join(directory, 'known.u16'), encodeSamples(Uint16Array.of(0x1234, 0x8001, 0, 0xffff)));
fs.writeFileSync(path.join(directory, 'known.json'), JSON.stringify({ format: 'logic16-le-v1', samplePeriodTicks: 24, sequenceStart: 700, droppedSamples: 2, sampleCount: 4 }, null, 2));
fs.writeFileSync(path.join(directory, 'half-sample.u16'), Uint8Array.of(1, 2, 3));
const long = new Uint16Array(MAX_SAMPLES + 9); long[0] = 0x1234; long[MAX_SAMPLES - 1] = 0xabcd; long[MAX_SAMPLES] = 0xffff;
fs.writeFileSync(path.join(directory, 'over-depth.u16'), encodeSamples(long));
fs.writeFileSync(path.join(directory, 'over-depth.json'), JSON.stringify({ samplePeriodNs: 1000, sequenceStart: 5, droppedSamples: 3, sampleCount: long.length }, null, 2));
console.log('已生成已知值、半样本、超深度及元数据夹具。');
