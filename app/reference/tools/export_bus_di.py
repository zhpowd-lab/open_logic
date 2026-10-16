"""将定频16路遥信的原始共享帧导出为VCD；保留实际读取入口时标，不插补。"""
import argparse
import csv
import json
from pathlib import Path
import struct
import zlib


def export(source, output, csv_output=None):
    source, output = Path(source), Path(output)
    if output.exists() or (csv_output and Path(csv_output).exists()):
        raise FileExistsError('导出文件已存在，不覆盖')
    previous_sequence = previous_ticks = first_ticks = last_raw = None
    session = period = None
    frames = min_interval = max_interval = 0
    csv_file = None
    try:
        if csv_output:
            csv_file = Path(csv_output).open('x', newline='', encoding='utf-8')
        with source.open('rb') as raw_file, output.open('x', encoding='ascii', newline='\n') as vcd:
            writer = csv.writer(csv_file) if csv_file else None
            if writer:
                writer.writerow(['sequence', 'counter_24mhz', 'relative_ns', 'di_raw', 'di_valid'])
            vcd.write('$comment 16 DI bits; software read-entry timestamps; no interpolation $end\n'
                      '$timescale 1ns $end\n$scope module bus_di $end\n')
            for bit in range(16):
                vcd.write(f'$var wire 1 d{bit} DI{bit+1:02d} $end\n')
            vcd.write('$upscope $end\n$enddefinitions $end\n')
            while True:
                frame = raw_file.read(256)
                if not frame:
                    break
                if len(frame) != 256:
                    raise ValueError('末尾共享帧不完整')
                sequence, = struct.unpack_from('<Q', frame, 4)
                epoch, generation, flags = struct.unpack_from('<III', frame, 12)
                tick, = struct.unpack_from('<Q', frame, 24)
                target, analog_count, words, mask = struct.unpack_from('<IHHI', frame, 48)
                digital = struct.unpack_from('<4H', frame, 224)
                valid = struct.unpack_from('<4H', frame, 232)
                crc, = struct.unpack_from('<I', frame, 240)
                if (not epoch or not generation or analog_count or mask or words != 1 or
                        flags & ((1 << 0) | (1 << 6) | (1 << 8) | (1 << 11)) or
                        not flags & (1 << 7) or not target or valid[0] != 0xffff or
                        any(digital[1:]) or any(valid[1:]) or zlib.crc32(frame[64:240]) != crc):
                    raise ValueError('不是有效的16位DI-only原始帧，或CRC错误')
                if frames:
                    if (session != (epoch, generation) or target != period or
                            sequence != previous_sequence + 1 or tick <= previous_ticks):
                        raise ValueError('会话、周期、序号或时标不连续')
                    interval = tick - previous_ticks
                    min_interval = min(min_interval or interval, interval)
                    max_interval = max(max_interval, interval)
                else:
                    if sequence != 1:
                        raise ValueError('捕获缺少会话首帧')
                    session, period, first_ticks = (epoch, generation), target, tick
                # 24MHz计数换算为ns只做整数四舍五入，原始计数保留在CSV。
                ns = ((tick - first_ticks) * 125 + 1) // 3
                vcd.write(f'#{ns}\n')
                for bit in range(16):
                    if last_raw is None or (last_raw ^ digital[0]) & (1 << bit):
                        vcd.write(f'{(digital[0] >> bit) & 1}d{bit}\n')
                if writer:
                    writer.writerow([sequence, tick, ns, f'0x{digital[0]:04x}', '0xffff'])
                previous_sequence, previous_ticks, last_raw = sequence, tick, digital[0]
                frames += 1
            if not frames:
                raise ValueError('捕获为空')
    finally:
        if csv_file:
            csv_file.close()
    return dict(frames=frames, bits=16, target_hz=24000000 / period,
                min_interval_ticks=min_interval, max_interval_ticks=max_interval,
                timestamps='software_read_entry_24mhz', physical_edges_tested=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--csv', type=Path)
    args = parser.parse_args()
    print(json.dumps(export(args.source, args.output, args.csv), ensure_ascii=False))


if __name__ == '__main__':
    main()
