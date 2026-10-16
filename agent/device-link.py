"""通过已核对的网口 2 读取控制台、传输隔离项目文件；不打印凭证。"""
import argparse
import ftplib
import json
from pathlib import Path
import re
import socket
import time

ADDRESS = "192.168.2.100"
PASSWORD_FILE = Path(r"U:\code\RT_MPU\src\dtu\dtu_5.0\sh_t153\tools\t153_password.txt")


def credential():
    return PASSWORD_FILE.read_text(encoding="utf-8-sig").strip()


def receive(connection, expected, timeout=12):
    result = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            block = connection.recv(65536)
        except socket.timeout:
            continue
        if not block:
            raise RuntimeError("装置控制台连接结束")
        # 本机服务使用 Telnet 协商；只应答选项，不把协商字节写入证据。
        for match in re.finditer(rb"\xff([\xfb-\xfe])(.)", block, re.S):
            action, option = match.group(1)[0], match.group(2)[0]
            response = 254 if action in (251, 252) else 252
            connection.sendall(bytes((255, response, option)))
        result += re.sub(rb"\xff[\xfb-\xfe].", b"", block, flags=re.S)
        if re.search(expected, result):
            return result
    raise TimeoutError("装置控制台响应超时")


def command(text):
    with socket.create_connection((ADDRESS, 6003), timeout=5) as connection:
        connection.settimeout(0.3)
        receive(connection, rb"login:\s*$")
        connection.sendall(b"root\r\n")
        receive(connection, rb"Password:\s*$")
        connection.sendall(credential().encode() + b"\r\n")
        receive(connection, rb"root@[^\r\n]*[#$] ?$")
        connection.sendall((text + "; printf '\\n__L16_RESULT:%d\\n' $?\r\n").encode())
        result = receive(connection, rb"[\r\n]__L16_RESULT:\d+[\r\n]", 40)
        connection.sendall(b"exit\r\n")
    result = result.decode("utf-8", errors="replace")
    result = re.sub(r"\x1b\[[0-9;]*m", "", result)
    result = re.sub(r"(?i)(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", "[MAC]", result)
    result = re.sub(r"(?i)\b[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\b", "[UUID]", result)
    print(result)
    match = re.search(r"[\r\n]__L16_RESULT:(\d+)", result)
    if not match or int(match[1]):
        raise RuntimeError("装置命令返回非零，请查看上方业务状态")


def transfer(local, remote, download=False):
    with ftplib.FTP() as ftp:
        ftp.connect(ADDRESS, 6001, timeout=12)
        ftp.login("root", credential())
        if download:
            with Path(local).open("wb") as output:
                ftp.retrbinary("RETR " + remote, output.write)
        else:
            import hashlib
            with Path(local).open("rb") as source:
                ftp.storbinary("STOR " + remote, source)
            check = hashlib.sha256()
            ftp.retrbinary("RETR " + remote, check.update)
            if check.digest() != hashlib.sha256(Path(local).read_bytes()).digest():
                raise RuntimeError("FTP 回读不一致")
    print(json.dumps({"file": Path(local).name, "remote": remote, "verified": not download}, ensure_ascii=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("command", "upload", "download"))
    parser.add_argument("value")
    parser.add_argument("remote", nargs="?")
    args = parser.parse_args()
    if args.action == "command":
        command(args.value)
    else:
        transfer(args.value, args.remote, args.action == "download")
