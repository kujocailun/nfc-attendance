import io
import json
import queue
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.error import URLError
from urllib.request import Request , urlopen
from http.server import ThreadingHTTPServer

sys.path.insert(0 , str(Path(__file__).resolve().parents[1]))
from database import Database
from device import Device , Protocol
from server import Application , handler
from validation import Problem , readcsv , validdate , validname , validuid

CSV = "time,uid,name,status\n2026-11-07 05:56:24,04-11-22-33,student,SAVED\n".encode()
CHINESE = "time,uid,name,status\n2026-11-08 10:01:02,01-02-03-04,测试成员,SAVED\n".encode()


class ValidationTests(unittest.TestCase):
    def test_uid(self):
        self.assertEqual(validuid("04-11-22-33") , "04-11-22-33")
        self.assertEqual(validuid("01-02-03-04-05-06-07") , "01-02-03-04-05-06-07")
        for value in (None , "FAAFD881" , "AA-BB-CC" , "AA-BB-CC-DD-EE" , "GG-AA-BB-CC"):
            with self.assertRaises(Problem):
                validuid(value)

    def test_name(self):
        self.assertEqual(validname("张 三") , "张 三")
        for value in ("" , "   " , "=1" , " @sum(1)" , "a,b" , 'a"b' , "a\nb" , "汉" * 17):
            with self.assertRaises(Problem):
                validname(value)

    def test_dates(self):
        validdate("2028-02-29")
        for value in ("2026-02-29" , "2026-11-31" , "1999-12-31" , "2026-1-01" , None):
            with self.assertRaises(Problem):
                validdate(value)

    def test_csv_exact(self):
        self.assertEqual(readcsv(CSV , "2026-11-07")[0]["uid"] , "04-11-22-33")
        self.assertEqual(readcsv(b"\xef\xbb\xbf" + CHINESE)[0]["name"] , "测试成员")
        with self.assertRaises(Problem):
            readcsv(CSV , "2026-11-08")

    def test_csv_reject_damage(self):
        for raw in (CSV[:-1] , CSV + CSV.split(b"\n")[1] + b"\n" , CSV.replace(b"SAVED" , b"ALREADY_TODAY") ,
                    CSV.replace(b"05:56:24" , b"25:56:24") , b"wrong\n" , CSV + b"\n" , b"\xff\n"):
            with self.assertRaises(Problem):
                readcsv(raw)


class DatabaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "attendance.sqlite3"
        self.db = Database(self.path)

    def tearDown(self):
        self.temp.cleanup()

    def test_idempotent_and_restart(self):
        self.assertEqual(self.db.ingest(readcsv(CSV) , "test")["added"] , 1)
        self.assertEqual(self.db.ingest(readcsv(CSV) , "test")["existing"] , 1)
        self.assertEqual(Database(self.path).overview("2026-11-07")["count"] , 1)

    def test_transaction_rollback(self):
        self.db.ingest(readcsv(CSV) , "test")
        rows = readcsv(CHINESE) + readcsv(CSV.replace(b"05:56:24" , b"06:56:24"))
        with self.assertRaises(Problem):
            self.db.ingest(rows , "test")
        self.assertEqual(self.db.overview("2026-11-08")["count"] , 0)

    def test_roster_notes_and_unknown(self):
        self.db.roster([("04-11-22-33" , "student")])
        self.db.edit("04-11-22-33" , {"student_no": "编号一" , "group_name": "测试分组" , "notes": "本机备注"})
        self.db.roster([("04-11-22-33" , "student")])
        self.assertEqual(self.db.members()[0]["notes"] , "本机备注")
        self.db.roster([])
        self.assertEqual(self.db.members()[0]["on_device"] , 0)
        with self.assertRaises(Problem):
            self.db.edit("00-00-00-00" , {})

    def test_complete_sync_and_live_invalidation(self):
        self.db.ingest(readcsv(CSV) , "sync" , "2026-11-07")
        self.assertIsNotNone(self.db.overview("2026-11-07")["sync"])
        raw = CSV.replace(b"04-11-22-33" , b"01-02-03-04")
        self.db.ingest(readcsv(raw) , "live")
        self.assertIsNone(self.db.overview("2026-11-07")["sync"])

    def test_filter_export_and_backup(self):
        self.db.ingest(readcsv(CSV) + readcsv(CHINESE) , "test")
        self.db.edit("04-11-22-33" , {"group_name": "=danger"})
        self.assertEqual(len(self.db.records("2026-11-07" , "2026-11-08" , "测试")) , 1)
        self.assertIn(b"'=danger" , self.db.export("2026-11-07" , "2026-11-08"))
        self.assertTrue(self.db.export("2026-11-07" , "2026-11-08").startswith(b"\xef\xbb\xbf"))
        target = Path(self.temp.name) / "backup.sqlite3"
        self.db.backup(target)
        self.assertEqual(Database(target).overview("2026-11-08")["total"] , 2)
        with self.assertRaises(Problem):
            self.db.records("2026-11-08" , "2026-11-07")


class ProtocolTests(unittest.TestCase):
    def test_fragmentation_and_unicode_byte_count(self):
        lines , events = [] , []
        parser = Protocol(lines.append , lambda k , v: events.append((k , v)))
        raw = b"hello\r\n[EXPORT-BEGIN] date=2026-11-08 bytes=" + str(len(CHINESE)).encode() + b"\n" + CHINESE + b"[EXPORT-END] result=PASS\nworld\n"
        for byte in raw:
            parser.feed(bytes([byte]))
        self.assertEqual(lines , ["hello" , "world"])
        self.assertEqual(events , [("export" , ("2026-11-08" , CHINESE))])

    def test_failure_marker_and_size(self):
        events = []
        parser = Protocol(lambda line: None , lambda k , v: events.append(k))
        parser.feed(b"[EXPORT-BEGIN] date=2026-11-07 bytes=4\nabc\n[EXPORT-END] result=FAIL\n")
        parser.feed(b"[EXPORT-BEGIN] date=2026-11-07 bytes=99999\n")
        self.assertEqual(events , ["export_error" , "export_error"])


class FakeSerial:
    def __init__(self , **kwargs):
        self.queue = queue.Queue()
        self.is_open = False
        self.writes = []
        self.roster = {"04-11-22-33": "student"}
        self.in_waiting = 0

    def open(self):
        self.is_open = True

    def close(self):
        self.is_open = False

    def flush(self):
        pass

    def read(self , size):
        try:
            return self.queue.get(timeout = 0.02)
        except queue.Empty:
            return b""

    def emit(self , text):
        raw = text if isinstance(text , bytes) else text.encode()
        for i in range(0 , len(raw) , 9):
            self.queue.put(raw[i:i + 9])

    def write(self , raw):
        self.writes.append(raw)
        cmd = raw.decode()
        if cmd == "r":
            self.emit("[NFC-ATTENDANCE] stage=08-attendance version=0.8.0\npn532_state=READY [PASS]\nrtc_state=VALID [PASS]\nsd_state=MOUNTED [PASS]\nattendance_today=2026-11-07 count=1 detail=NONE\nSend r to report, other commands\n")
        elif cmd == "l":
            self.emit(f"[MEMBERS] ready=1 count={len(self.roster)} limit=128\n")
            for uid , name in self.roster.items():
                self.emit(f"[MEMBER] uid={uid} name={name}\n")
        elif cmd == "E2026-11-07\n":
            self.emit(f"[EXPORT-BEGIN] date=2026-11-07 bytes={len(CSV)}\n".encode() + CSV + b"[EXPORT-END] result=PASS\n")
        elif cmd.startswith("E"):
            self.emit("[EXPORT] NO_VALID_FILE detail=NONE\n")
        elif cmd.startswith("N"):
            uid , name = cmd[1:].rstrip("\n").split(",")
            if uid in self.roster and self.roster[uid] != name:
                self.emit(f"[BIND] UID_CONFLICT uid={uid} name={self.roster[uid]}\n")
            else:
                self.roster[uid] = name
                self.emit(f"[BIND] SAVED uid={uid} name={name} members={len(self.roster)}\n")
        elif cmd.startswith("T"):
            self.emit("[RTC-SYNC] time=2026-11-08 09:00:00 timezone=UTC+08:00 [PASS]\n")
        return len(raw)


class DeviceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.db = Database(Path(self.temp.name) / "db.sqlite3")
        self.device = Device(self.db)
        self.portpatch = patch("device.list_ports.comports" , return_value = [SimpleNamespace(device = "COM_TEST" , description = "fake")])
        self.serialpatch = patch("device.serial.Serial" , FakeSerial)
        self.portpatch.start()
        self.serialpatch.start()

    def tearDown(self):
        self.device.disconnect()
        self.portpatch.stop()
        self.serialpatch.stop()
        self.temp.cleanup()

    def test_real_command_flow_through_mock_transport(self):
        self.device.connect("COM_TEST")
        self.assertEqual(self.db.members()[0]["name"] , "student")
        self.assertTrue(self.device.snapshot()["responsive"])
        self.assertEqual(self.device.sync("2026-11-07")["added"] , 1)
        self.assertEqual(self.device.sync("2026-11-07")["existing"] , 1)
        self.device.bind("01-02-03-04" , "测试成员")
        self.assertEqual(len(self.db.members()) , 2)
        with self.assertRaises(Problem):
            self.device.bind("04-11-22-33" , "改名")
        with self.assertRaises(Problem):
            self.device.sync("2026-11-08")
        self.assertEqual(self.db.overview("2026-11-07")["count"] , 1)
        self.device.clock()
        self.assertFalse(self.device.link.dtr)
        self.assertFalse(self.device.link.rts)

    def test_duplicate_event_does_not_forge_original_time(self):
        self.device.online("[ATTENDANCE] state=ALREADY_TODAY time=2026-11-07 10:00:00 uid=04-11-22-33 name=student detail=NONE")
        self.assertEqual(self.db.overview("2026-11-07")["count"] , 0)
        self.device.online("[ATTENDANCE] state=SAVED time=2026-11-07 05:56:24 uid=04-11-22-33 name=student detail=NONE")
        self.device.online("[ATTENDANCE] state=SAVED time=2026-11-07 05:56:24 uid=04-11-22-33 name=student detail=NONE")
        self.assertEqual(self.db.overview("2026-11-07")["count"] , 1)

    def test_timeout_and_missing_port(self):
        with self.assertRaises(Problem):
            self.device.connect("DOES_NOT_EXIST")
        self.device.connect("COM_TEST")
        with self.device.command , self.assertRaises(Problem):
            self.device._request(b"?" , lambda k , v: None , timeout = 0.05)


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.app = Application(Path(self.temp.name) / "db.sqlite3")
        self.server = ThreadingHTTPServer(("127.0.0.1" , 0) , handler(self.app))
        self.thread = threading.Thread(target = self.server.serve_forever , daemon = True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.app.device.disconnect()
        self.temp.cleanup()

    def request(self , path , raw = None , headers = None):
        req = Request(self.base + path , data = raw , headers = headers or {})
        return urlopen(req , timeout = 5)

    def test_static_and_host_protection(self):
        with self.request("/") as response:
            self.assertIn("签到总览" , response.read().decode())
            self.assertIn("frame-ancestors 'none'" , response.headers["Content-Security-Policy"])
        for path in ("/app.js" , "/style.css" , "/favicon.svg"):
            with self.request(path) as response:
                self.assertEqual(response.status , 200)
        with self.assertRaises(HTTPError) as error:
            self.request("/api/bootstrap" , headers = {"Host": "evil.invalid"})
        self.assertEqual(error.exception.code , 403)

    def test_import_query_export_and_backup(self):
        with self.request("/api/bootstrap") as response:
            token = json.load(response)["token"]
        headers = {"X-Admin-Token": token}
        with self.request("/api/import" , CSV , headers) as response:
            self.assertEqual(json.load(response)["added"] , 1)
        with self.request("/api/records?start=2026-11-07&end=2026-11-07") as response:
            self.assertEqual(json.load(response)["total"] , 1)
        with self.request("/api/export?start=2026-11-07&end=2026-11-07") as response:
            self.assertIn(b"student" , response.read())
        with self.request("/api/backup") as response:
            self.assertTrue(response.read().startswith(b"SQLite format 3"))
        with self.assertRaises(HTTPError) as error:
            self.request("/api/import" , CSV)
        self.assertEqual(error.exception.code , 403)
        with self.assertRaises(HTTPError) as error:
            self.request("/api/import" , CSV , {**headers , "Origin": "http://evil.invalid"})
        self.assertEqual(error.exception.code , 403)
        with self.assertRaises(HTTPError) as error:
            self.request("/api/records?start=2026-11-09&end=2026-11-07")
        self.assertEqual(error.exception.code , 400)


class LifecycleTests(unittest.TestCase):
    def test_server_stop_and_restart_retains_database(self):
        with tempfile.TemporaryDirectory() as folder:
            with socket.socket() as probe:
                probe.bind(("127.0.0.1" , 0))
                port = probe.getsockname()[1]
            base = f"http://127.0.0.1:{port}"
            database = str(Path(folder) / "db.sqlite3")
            for cycle in range(2):
                process = subprocess.Popen([sys.executable , str(Path(__file__).resolve().parents[1] / "server.py") ,
                                            "--port" , str(port) , "--db" , database] ,
                                           stdout = subprocess.PIPE , stderr = subprocess.PIPE)
                try:
                    deadline = time.monotonic() + 5
                    while True:
                        try:
                            with urlopen(base + "/api/bootstrap" , timeout = 1) as response:
                                token = json.load(response)["token"]
                            break
                        except URLError:
                            if time.monotonic() > deadline:
                                self.fail("server did not start")
                            time.sleep(0.05)
                    headers = {"X-Admin-Token": token}
                    if cycle == 0:
                        with urlopen(Request(base + "/api/import" , data = CSV , headers = headers) , timeout = 3) as response:
                            self.assertEqual(json.load(response)["added"] , 1)
                    else:
                        with urlopen(base + "/api/overview?day=2026-11-07" , timeout = 3) as response:
                            self.assertEqual(json.load(response)["count"] , 1)
                    with urlopen(Request(base + "/api/shutdown" , data = b"{}" , headers = headers) , timeout = 3) as response:
                        self.assertEqual(response.status , 200)
                    out , error = process.communicate(timeout = 5)
                    self.assertEqual(process.returncode , 0 , error.decode(errors = "replace"))
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate(timeout = 3)


if __name__ == "__main__":
    unittest.main(verbosity = 2)
