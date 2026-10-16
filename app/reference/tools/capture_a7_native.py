#!/usr/bin/env python3
"""通过已有只读状态页保存完整A7诊断；不CLAIM、不ARM，控制端占用时明确失败。"""
import argparse
import mmap
import os
from pathlib import Path
import struct
import time


def capture():
    fd=os.open('/dev/rt_mpu_amp_acq',os.O_RDWR|os.O_CLOEXEC)
    try:
        with mmap.mmap(fd,4096,flags=mmap.MAP_SHARED,prot=mmap.PROT_READ,offset=0x442f0000) as region:
            for _ in range(20):
                before=struct.unpack_from('<I',region,12)[0]
                if not before&1:
                    page=region[:]
                    after=struct.unpack_from('<I',region,12)[0]
                    if before==after==struct.unpack_from('<I',page,12)[0]:
                        if struct.unpack_from('<3I',page,0)!=(0x31435141,2,128):
                            raise RuntimeError('A7诊断版本/布局不匹配')
                        return page
                time.sleep(.001)
    finally:
        os.close(fd)
    raise RuntimeError('A7状态页未取得一致快照，保留现场')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    page=capture()
    with args.output.open('xb') as stream:stream.write(page)
    print('Saved coherent 4096-byte A7 native status page; no claim/start commands')


if __name__=='__main__':main()
