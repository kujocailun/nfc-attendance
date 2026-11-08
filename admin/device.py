import re
import threading
import time
from collections import deque

import serial
from serial.tools import list_ports

from validation import Problem , nowtext , readcsv , today , validdate , validname , validuid

EVENT = re.compile(r"^\[ATTENDANCE\] state=(\S+) time=(.*?) uid=(.*?) name=(.*?) detail=(.*)$")


class Protocol:
    def __init__(self , online , event):
        self.online = online
        self.event = event
        self.buffer = bytearray()
        self.export = None
        self.export_data = None

    def feed(self , data):
        self.buffer.extend(data)
        if len(self.buffer) > 131072:
            self.buffer.clear()
            self.export = None
            self.export_data = None
            raise Problem("串口数据超过协议上限" , 502)
        while True:
            if self.export and self.export_data is None:
                size = self.export[1]
                if len(self.buffer) < size:
                    return
                self.export_data = bytes(self.buffer[:size])
                del self.buffer[:size]
            index = self.buffer.find(b"\n")
            if index < 0:
                return
            raw = bytes(self.buffer[:index + 1])
            del self.buffer[:index + 1]
            line = raw.decode("utf-8" , errors = "replace").rstrip("\r\n")
            if self.export:
                if line != "[EXPORT-END] result=PASS":
                    self.export = None
                    self.export_data = None
                    self.event("export_error" , "导出末尾校验失败")
                    continue
                date = self.export[0]
                payload = self.export_data
                self.export = None
                self.export_data = None
                self.event("export" , (date , payload))
                continue
            match = re.fullmatch(r"\[EXPORT-BEGIN\] date=(\d{4}-\d{2}-\d{2}) bytes=(\d+)" , line)
            if match:
                size = int(match[2])
                if not 1 <= size <= 32768:
                    self.event("export_error" , "导出声明长度超出范围")
                    continue
                self.export = (match[1] , size)
                continue
            self.online(line)


class Device:
    def __init__(self , db):
        self.db = db
        self.lock = threading.RLock()
        self.command = threading.Lock()
        self.condition = threading.Condition(self.lock)
        self.stop = threading.Event()
        self.link = None
        self.thread = None
        self.port = ""
        self.error = ""
        self.fields = {}
        self.logs = deque(maxlen = 120)
        self.events = deque(maxlen = 40)
        self.replies = deque(maxlen = 512)
        self.seq = 0
        self.last_seen = 0
        self.last_card = None
        self.last_sync = ""
        self.protocol = Protocol(self.online , self.receive)
        self.sync_due = False
        self.report_lines = None

    def ports(self):
        return [{"port": p.device , "description": p.description} for p in list_ports.comports()]

    def snapshot(self):
        with self.lock:
            live = bool(self.link and self.link.is_open and self.last_seen and time.monotonic() - self.last_seen < 18)
            return {"connected": bool(self.link and self.link.is_open) , "responsive": live , "port": self.port ,
                    "error": self.error , "fields": dict(self.fields) , "last_card": self.last_card ,
                    "last_sync": self.last_sync , "events": list(self.events) , "logs": list(self.logs)[-40:]}

    def receive(self , kind , value):
        with self.condition:
            self.seq += 1
            self.replies.append((self.seq , kind , value))
            self.condition.notify_all()

    def online(self , line):
        with self.lock:
            self.last_seen = time.monotonic()
            self.logs.append({"time": nowtext()[11:] , "text": line})
            if line.startswith("[NFC-ATTENDANCE]"):
                self.report_lines = []
            if self.report_lines is not None:
                self.report_lines.append(line)
            if line.startswith("Send r to report") and self.report_lines is not None:
                self.receive("report" , "\n".join(self.report_lines))
                self.report_lines = None
            for key in ("pn532_state" , "oled_state" , "rtc_state" , "sd_state" , "attendance_state" , "registry_ready" , "members" , "count" , "version" ,
                        "network_id" , "network_state" , "portal" , "uploaded_acks" , "failures"):
                match = re.search(r"(?:^|\s)" + key + r"=(\S+)" , line)
                if match:
                    self.fields[key] = match[1]
            for key in ("rtc_local" , "attendance_today"):
                if line.startswith(key + "="):
                    self.fields[key] = line.split("=" , 1)[1].split(" count=" , 1)[0]
            if line.startswith("[CARD]"):
                match = re.search(r"uid=(\S+)" , line)
                if match:
                    self.last_card = {"uid": match[1] , "seen_at": nowtext()}
            match = EVENT.fullmatch(line)
            if match:
                state , stamp , uid , name , detail = match.groups()
                self.events.appendleft({"state": state , "time": stamp , "uid": uid , "name": name , "detail": detail})
                if state == "SAVED":
                    packet = ("time,uid,name,status\n" + f"{stamp},{uid},{name},SAVED\n").encode("utf-8")
                    try:
                        self.db.ingest(readcsv(packet) , "USB 实时")
                    except Problem as exc:
                        self.error = str(exc)
                if state in ("SAVED" , "ALREADY_TODAY"):
                    self.sync_due = True
            self.receive("line" , line)

    def _reader(self):
        try:
            while not self.stop.is_set():
                chunk = self.link.read(max(1 , min(self.link.in_waiting , 4096)))
                if chunk:
                    self.protocol.feed(chunk)
        except Exception as exc:
            if not self.stop.is_set():
                with self.condition:
                    self.error = "串口连接中断，请重新连接。" + str(exc)
                    if self.link:
                        self.link.close()
                    self.condition.notify_all()

    def _request(self , data , predicate , timeout = 8):
        with self.condition:
            if not self.link or not self.link.is_open:
                raise Problem("请先连接终端" , 409)
            mark = self.seq
            try:
                self.link.write(data)
                self.link.flush()
            except serial.SerialException as exc:
                raise Problem("串口写入失败，请重新连接" , 502) from exc
            deadline = time.monotonic() + timeout
            while True:
                for seq , kind , value in self.replies:
                    if seq > mark:
                        mark = seq
                        result = predicate(kind , value)
                        if result is not None:
                            return result
                left = deadline - time.monotonic()
                if left <= 0:
                    raise Problem("终端回复超时。请检查供电与接线；写入操作请先同步名单确认结果。" , 504)
                if not self.link.is_open:
                    raise Problem("终端已断开" , 502)
                self.condition.wait(min(left , 0.25))

    def connect(self , port):
        with self.command:
            if not isinstance(port , str) or not port:
                raise Problem("请先选择串口")
            self._disconnect()
            available = {p["port"] for p in self.ports()}
            if port not in available:
                raise Problem("串口不存在，请刷新列表" , 404)
            link = serial.Serial(port = None , baudrate = 115200 , timeout = 0.1 , write_timeout = 2)
            link.dtr = False
            link.rts = False
            link.port = port
            try:
                link.open()
            except serial.SerialException as exc:
                raise Problem("无法打开串口。请关闭其他串口监视器后重试。" , 409) from exc
            with self.lock:
                self.link = link
                self.port = port
                self.error = ""
                self.fields = {}
                self.last_seen = 0
                self.last_card = None
                self.events.clear()
                self.logs.clear()
                self.replies.clear()
                self.report_lines = None
                self.protocol = Protocol(self.online , self.receive)
                self.stop.clear()
            self.thread = threading.Thread(target = self._reader , daemon = True)
            self.thread.start()
            try:
                self._request(b"r" , lambda k , v: v if k == "report" else None)
                if self.fields.get("version") not in {"0.8.0" , "0.9.0" , "0.10.0"}:
                    raise Problem("当前后台兼容终端固件 v0.8.0 / v0.9.0 / v0.10.0" , 409)
                self._members()
            except Exception:
                self._disconnect()
                raise
            self.db.audit("连接终端" , port)
            return self.snapshot()

    def _disconnect(self):
        self.stop.set()
        if self.thread and self.thread.is_alive():
            self.thread.join(timeout = 2)
        with self.lock:
            if self.link:
                self.link.close()
            self.link = None
            self.thread = None

    def disconnect(self):
        with self.command:
            self._disconnect()
        return self.snapshot()

    def _members(self):
        rows = []
        expected = None

        def accept(kind , value):
            nonlocal expected
            if kind != "line":
                return None
            header = re.fullmatch(r"\[MEMBERS\] ready=(\d+) count=(\d+) limit=128" , value)
            if header:
                if header[1] != "1" or int(header[2]) > 128:
                    raise Problem("终端名单尚未就绪" , 409)
                expected = int(header[2])
                return [] if expected == 0 else None
            row = re.fullmatch(r"\[MEMBER\] uid=(\S+) name=(.*)" , value)
            if row and expected is not None:
                uid = validuid(row[1])
                name = validname(row[2])
                if uid in {r[0] for r in rows}:
                    raise Problem("终端名单含重复卡号" , 502)
                rows.append((uid , name))
                if len(rows) == expected:
                    return rows
            return None

        result = self._request(b"l" , accept)
        self.db.roster(result)
        return len(result)

    def members(self):
        with self.command:
            return {"count": self._members()}

    def sync(self , day):
        validdate(day)
        with self.command:
            def accept(kind , value):
                if kind == "export":
                    if value[0] != day:
                        raise Problem("终端返回的日期与请求不符" , 502)
                    return value[1]
                if kind == "export_error":
                    raise Problem(value , 502)
                if kind == "line" and value.startswith("[EXPORT]"):
                    raise Problem("终端未提供该日期的有效文件。可能尚无人签到，也可能文件不可用；本地已有记录保留。" , 409)
                return None
            raw = self._request(("E" + day + "\n").encode("ascii") , accept)
            result = self.db.ingest(readcsv(raw , day) , "USB 文件同步" , day)
            with self.lock:
                self.last_sync = nowtext()
                self.sync_due = False
            return result

    def bind(self , uid , name):
        uid = validuid(uid)
        name = validname(name)
        with self.command:
            reply = self._request(("N" + uid + "," + name + "\n").encode("utf-8") ,
                                  lambda k , v: v if k == "line" and v.startswith("[BIND]") else None)
            match = re.fullmatch(r"\[BIND\] (SAVED|ALREADY_BOUND) uid=(\S+) name=(.*?)(?: members=\d+)?" , reply)
            if not match or match[2] != uid or match[3] != name:
                if "UID_CONFLICT" in reply:
                    raise Problem("该卡已经绑定其他姓名，终端保留原绑定。请同步名单核对。" , 409)
                raise Problem("终端未确认登记成功。" + reply , 409)
            self._members()
            self.db.audit("登记卡片" , uid + " / " + name)
            return {"message": "终端已保存并确认名单"}

    def clock(self):
        with self.command:
            stamp = nowtext()
            cmd = "T" + re.sub(r"[- :]" , "" , stamp) + "\n"
            reply = self._request(cmd.encode("ascii") , lambda k , v: v if k == "line" and v.startswith("[RTC-SYNC]") else None)
            if not reply.endswith("[PASS]"):
                raise Problem("终端校时失败。" + reply , 409)
            self.db.audit("终端校时" , stamp)
            return {"message": "已按电脑的北京时间校时" , "time": stamp}

    def maintenance(self , shutdown):
        last = 0
        while not shutdown.wait(2):
            state = self.snapshot()
            if not state["responsive"]:
                continue
            if self.sync_due or time.monotonic() - last > 60:
                last = time.monotonic()
                day = state["fields"].get("attendance_today" , "")
                if day and re.fullmatch(r"20\d{2}-\d{2}-\d{2}" , day):
                    try:
                        self.sync(day)
                    except Problem:
                        self.sync_due = False
