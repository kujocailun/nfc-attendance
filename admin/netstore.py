import hashlib
import json
import re
import secrets
import time

from database import Database
from validation import Problem , field , nowtext , readcsv , validname , validuid

OPERATIONS = {"clock": "RTC_READBACK_OK" , "roster": "ROSTER_UPLOADED" , "rescan": "HISTORY_SCAN_ACKED"}


def digest(value):
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def eventid(device , day , uid):
    return digest(device + "\n" + day + "\n" + uid)


def deviceid(value):
    if not isinstance(value , str) or not re.fullmatch(r"s3-[0-9a-f]{12}" , value):
        raise Problem("设备编号无效")
    return value


class NetStore(Database):
    def __init__(self , path):
        super().__init__(path)
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS net_devices(
                    id TEXT PRIMARY KEY, label TEXT NOT NULL, secret_hash TEXT NOT NULL,
                    revoked INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL,
                    seen REAL NOT NULL DEFAULT 0, telemetry TEXT NOT NULL DEFAULT '{}'
                );
                CREATE TABLE IF NOT EXISTS net_pairs(
                    code_hash TEXT PRIMARY KEY, label TEXT NOT NULL, expires REAL NOT NULL,
                    claimed_id TEXT, claimed_hash TEXT
                );
                CREATE TABLE IF NOT EXISTS net_events(
                    id TEXT PRIMARY KEY, device_id TEXT NOT NULL, day TEXT NOT NULL,
                    uid TEXT NOT NULL, time TEXT NOT NULL, name TEXT NOT NULL,
                    UNIQUE(device_id,day,uid)
                );
                CREATE TABLE IF NOT EXISTS net_commands(
                    id TEXT PRIMARY KEY, device_id TEXT NOT NULL, kind TEXT NOT NULL,
                    payload TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'pending',
                    created_at TEXT NOT NULL, completed_at TEXT, result TEXT
                );
            """)
            columns = {row["name"] for row in db.execute("PRAGMA table_info(net_commands)")}
            if "started_at" not in columns:
                db.execute("ALTER TABLE net_commands ADD COLUMN started_at TEXT")

    def makepair(self , label):
        label = field(label , 40) or "实验室签到终端"
        code = secrets.token_hex(8).upper()
        with self.connect() as db:
            db.execute("DELETE FROM net_pairs WHERE expires<?" , (time.time() ,))
            db.execute("INSERT INTO net_pairs(code_hash,label,expires) VALUES(?,?,?)" ,
                       (digest(code) , label , time.time() + 600))
        self.audit("生成设备配对码" , label)
        return {"code": code , "expires_in": 600}

    def pair(self , data):
        ident = deviceid(data.get("device_id"))
        secret = data.get("secret")
        code = data.get("code")
        if not isinstance(secret , str) or not re.fullmatch(r"[0-9a-f]{64}" , secret):
            raise Problem("设备密钥必须由安全随机源生成")
        if not isinstance(code , str) or not re.fullmatch(r"[0-9A-F]{16}" , code):
            raise Problem("配对码无效" , 401)
        hashed = digest(secret)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM net_pairs WHERE code_hash=?" , (digest(code) ,)).fetchone()
            if not row or row["expires"] < time.time():
                raise Problem("配对码无效或已过期" , 401)
            if row["claimed_id"]:
                if row["claimed_id"] != ident or row["claimed_hash"] != hashed:
                    raise Problem("配对码已使用" , 409)
                return {"device_id": ident , "paired": True}
            other = db.execute("SELECT id FROM net_devices WHERE revoked=0 AND id<>?" , (ident ,)).fetchone()
            if other:
                raise Problem("第一版支持一台联网终端，请先撤销旧终端" , 409)
            db.execute("INSERT INTO net_devices(id,label,secret_hash,created_at) VALUES(?,?,?,?) "
                       "ON CONFLICT(id) DO UPDATE SET label=excluded.label,secret_hash=excluded.secret_hash,revoked=0" ,
                       (ident , row["label"] , hashed , nowtext()))
            db.execute("UPDATE net_pairs SET claimed_id=?,claimed_hash=? WHERE code_hash=?" , (ident , hashed , digest(code)))
            db.execute("INSERT INTO audit(time,action,detail) VALUES(?,?,?)" , (nowtext() , "配对联网终端" , ident))
        return {"device_id": ident , "paired": True}

    def authenticate(self , ident , secret):
        deviceid(ident)
        if not isinstance(secret , str) or len(secret) != 64:
            raise Problem("设备凭据无效" , 401)
        with self.connect() as db:
            row = db.execute("SELECT secret_hash,revoked FROM net_devices WHERE id=?" , (ident ,)).fetchone()
        if not row or row["revoked"] or not secrets.compare_digest(row["secret_hash"] , digest(secret)):
            raise Problem("设备凭据无效或已撤销" , 401)

    def devices(self):
        with self.connect() as db:
            rows = db.execute("SELECT id,label,revoked,created_at,seen,telemetry FROM net_devices ORDER BY created_at").fetchall()
            commands = [dict(row) for row in db.execute("SELECT * FROM net_commands ORDER BY "
                        "(state IN ('pending','running')) DESC,created_at DESC,id DESC LIMIT 30")]
        result = []
        for row in rows:
            item = dict(row)
            item["online"] = not item["revoked"] and time.time() - item["seen"] < 180
            item["telemetry"] = json.loads(item["telemetry"])
            item["capabilities"] = self.capabilities(item["telemetry"])
            result.append(item)
        return {"devices": result , "commands": commands}

    def revoke(self , ident):
        deviceid(ident)
        with self.connect() as db:
            if not db.execute("UPDATE net_devices SET revoked=1 WHERE id=?" , (ident ,)).rowcount:
                raise Problem("设备不存在" , 404)
            db.execute("DELETE FROM net_pairs WHERE claimed_id=?" , (ident ,))
        self.audit("撤销设备凭据" , ident)
        return {"message": "设备密钥已撤销，可重新生成配对码配置终端"}

    def heartbeat(self , ident , data):
        allowed = {"version" , "rtc" , "sd" , "pn532" , "oled" , "registry" , "rssi" , "pending" , "members" , "today" , "last_uid" , "state"}
        payload = {}
        for key in allowed:
            value = data.get(key)
            if isinstance(value , (str , int , bool)) and len(str(value)) <= 100:
                payload[key] = value
        payload["capabilities"] = self.capabilities(data)
        with self.connect() as db:
            db.execute("UPDATE net_devices SET seen=?,telemetry=? WHERE id=?" ,
                       (time.time() , json.dumps(payload , ensure_ascii = False) , ident))
        return {"ok": True , "time": nowtext()}

    @staticmethod
    def capabilities(telemetry):
        values = telemetry.get("capabilities" , [])
        return [kind for kind in OPERATIONS if isinstance(values , list) and kind in values]

    def upload(self , ident , data):
        events = data.get("events")
        if not isinstance(events , list) or not 1 <= len(events) <= 32:
            raise Problem("每批应含 1～32 条记录")
        checked = []
        for row in events:
            if not isinstance(row , dict) or set(row) != {"event_id" , "time" , "uid" , "name" , "status"}:
                raise Problem("记录字段无效")
            # 复用已经验收的 CSV 校验，防止网络接口放宽原规则。
            import csv
            import io
            out = io.StringIO()
            writer = csv.writer(out , lineterminator = "\n")
            writer.writerow(["time" , "uid" , "name" , "status"])
            writer.writerow([row[k] for k in ("time" , "uid" , "name" , "status")])
            item = readcsv(out.getvalue().encode())[0]
            expected = eventid(ident , item["time"][:10] , item["uid"])
            if row["event_id"] != expected:
                raise Problem("事件编号与设备、日期、卡号不符")
            item["event_id"] = expected
            checked.append(item)
        added = 0
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            for row in checked:
                day = row["time"][:10]
                old = db.execute("SELECT time,name,status FROM records WHERE day=? AND uid=?" , (day , row["uid"])).fetchone()
                if old and tuple(old) != (row["time"] , row["name"] , "SAVED"):
                    raise Problem("上传记录与已存历史冲突，整批保留待处理" , 409)
                oldevent = db.execute("SELECT time,name FROM net_events WHERE id=?" , (row["event_id"] ,)).fetchone()
                if oldevent and tuple(oldevent) != (row["time"] , row["name"]):
                    raise Problem("事件编号已存在但内容不同" , 409)
                if not old:
                    db.execute("INSERT INTO records VALUES(?,?,?,?,?,?,?)" ,
                               (day , row["uid"] , row["time"] , row["name"] , "SAVED" , "Wi-Fi " + ident , nowtext()))
                    added += 1
                db.execute("INSERT OR IGNORE INTO net_events VALUES(?,?,?,?,?,?)" ,
                           (row["event_id"] , ident , day , row["uid"] , row["time"] , row["name"]))
                db.execute("INSERT OR IGNORE INTO members(uid,name,updated_at) VALUES(?,?,?)" , (row["uid"] , row["name"] , nowtext()))
            db.execute("UPDATE net_devices SET seen=? WHERE id=?" , (time.time() , ident))
        return {"acked": [row["event_id"] for row in checked] , "added": added}

    def queuebind(self , ident , uid , name):
        deviceid(ident)
        uid , name = validuid(uid) , validname(name)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            if not db.execute("SELECT 1 FROM net_devices WHERE id=? AND revoked=0" , (ident ,)).fetchone():
                raise Problem("设备不存在或已撤销" , 404)
            old = db.execute("SELECT name FROM members WHERE uid=? AND on_device=1" , (uid ,)).fetchone()
            if old and old["name"] != name:
                raise Problem("已绑定姓名不能直接修改" , 409)
            if db.execute("SELECT COUNT(*) FROM net_commands WHERE device_id=? AND state IN ('pending','running')" , (ident ,)).fetchone()[0] >= 128:
                raise Problem("待执行命令过多，请等待终端联网" , 409)
            key = secrets.token_hex(16)
            db.execute("INSERT INTO net_commands(id,device_id,kind,payload,created_at) VALUES(?,?,'bind',?,?)" ,
                       (key , ident , json.dumps({"uid": uid , "name": name} , ensure_ascii = False) , nowtext()))
        return {"id": key , "state": "pending" , "message": "登记请求已排队，终端写入并读回后才算完成"}

    def queueoperation(self , ident , kind):
        deviceid(ident)
        if not isinstance(kind , str) or kind not in OPERATIONS:
            raise Problem("不支持的终端操作")
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT telemetry FROM net_devices WHERE id=? AND revoked=0" , (ident ,)).fetchone()
            if not row:
                raise Problem("设备不存在或已撤销" , 404)
            if kind not in self.capabilities(json.loads(row["telemetry"])):
                raise Problem("终端尚未上报此能力，请先升级至 v0.10.0 并等待状态更新" , 409)
            old = db.execute("SELECT id,state FROM net_commands WHERE device_id=? AND kind=? AND state IN ('pending','running') "
                             "ORDER BY created_at,id LIMIT 1" , (ident , kind)).fetchone()
            if old:
                return {**dict(old) , "message": "已有相同任务，正在等待终端确认"}
            if db.execute("SELECT COUNT(*) FROM net_commands WHERE device_id=? AND state IN ('pending','running')" , (ident ,)).fetchone()[0] >= 128:
                raise Problem("待执行命令过多，请等待终端联网" , 409)
            key = secrets.token_hex(16)
            db.execute("INSERT INTO net_commands(id,device_id,kind,payload,created_at) VALUES(?,?,?,'{}',?)" ,
                       (key , ident , kind , nowtext()))
            db.execute("INSERT INTO audit(time,action,detail) VALUES(?,?,?)" , (nowtext() , "提交远程操作" , ident + " " + kind))
        return {"id": key , "state": "pending" , "message": "任务已排队，终端联网执行并回执后才会显示完成"}

    def commands(self , ident):
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            device = db.execute("SELECT telemetry FROM net_devices WHERE id=? AND revoked=0" , (ident ,)).fetchone()
            if not device:
                raise Problem("设备不存在或已撤销" , 404)
            supported = ["bind"] + self.capabilities(json.loads(device["telemetry"]))
            placeholders = ",".join("?" for _ in supported)
            rows = db.execute("SELECT id,kind,payload FROM net_commands WHERE device_id=? AND state IN ('pending','running') "
                              f"AND kind IN ({placeholders}) ORDER BY created_at,id LIMIT 1" , (ident , *supported)).fetchall()
            for row in rows:
                db.execute("UPDATE net_commands SET state='running',started_at=COALESCE(started_at,?) WHERE id=?" , (nowtext() , row["id"]))
        return {"commands": [{"id": r["id"] , "kind": r["kind"] , **json.loads(r["payload"])} for r in rows]}

    def ackcommand(self , ident , data):
        key , ok = data.get("id") , data.get("ok")
        if not isinstance(key , str) or not isinstance(ok , bool):
            raise Problem("命令回执无效")
        detail = field(data.get("detail" , "") , 120)
        state = "done" if ok else "failed"
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM net_commands WHERE id=? AND device_id=?" , (key , ident)).fetchone()
            if not row:
                raise Problem("命令不存在" , 404)
            if row["state"] not in {"pending" , "running"}:
                if row["state"] != state:
                    raise Problem("回执与已确认状态冲突" , 409)
                return {"acked": key}
            if ok and row["kind"] in OPERATIONS and detail != OPERATIONS[row["kind"]]:
                raise Problem("终端回执缺少操作完成确认" , 409)
            if ok and row["kind"] == "bind":
                payload = json.loads(row["payload"])
                db.execute("INSERT INTO members(uid,name,on_device,updated_at) VALUES(?,?,1,?) "
                           "ON CONFLICT(uid) DO UPDATE SET name=excluded.name,on_device=1,updated_at=excluded.updated_at" ,
                           (payload["uid"] , payload["name"] , nowtext()))
            db.execute("UPDATE net_commands SET state=?,completed_at=?,result=? WHERE id=?" , (state , nowtext() , detail , key))
        return {"acked": key}

    def netroster(self , ident , data):
        rows = data.get("members")
        if not isinstance(rows , list) or len(rows) > 128:
            raise Problem("终端名单无效")
        members = []
        for row in rows:
            if not isinstance(row , dict):
                raise Problem("名单行无效")
            members.append((validuid(row.get("uid")) , validname(row.get("name"))))
        if len({u for u , n in members}) != len(members):
            raise Problem("名单含重复卡号")
        self.roster(members)
        return {"count": len(members)}
