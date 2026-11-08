import csv
import io
import sqlite3
from contextlib import closing , contextmanager
from datetime import datetime , timedelta
from pathlib import Path

from validation import Problem , field , nowtext , safecell , validdate , validuid


class Database:
    def __init__(self , path):
        self.path = Path(path)
        self.path.parent.mkdir(parents = True , exist_ok = True)
        with self.connect() as db:
            db.executescript("""
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS members(
                    uid TEXT PRIMARY KEY, name TEXT NOT NULL,
                    student_no TEXT NOT NULL DEFAULT '', group_name TEXT NOT NULL DEFAULT '',
                    notes TEXT NOT NULL DEFAULT '', on_device INTEGER NOT NULL DEFAULT 0,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS records(
                    day TEXT NOT NULL, uid TEXT NOT NULL, time TEXT NOT NULL,
                    name TEXT NOT NULL, status TEXT NOT NULL, source TEXT NOT NULL,
                    imported_at TEXT NOT NULL, PRIMARY KEY(day,uid)
                );
                CREATE INDEX IF NOT EXISTS records_time ON records(time);
                CREATE TABLE IF NOT EXISTS syncs(
                    day TEXT PRIMARY KEY, synced_at TEXT NOT NULL, count INTEGER NOT NULL
                );
                CREATE TABLE IF NOT EXISTS audit(
                    id INTEGER PRIMARY KEY, time TEXT NOT NULL, action TEXT NOT NULL,
                    detail TEXT NOT NULL
                );
            """)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path , timeout = 10)
        db.row_factory = sqlite3.Row
        try:
            with db:
                yield db
        finally:
            db.close()

    def audit(self , action , detail):
        with self.connect() as db:
            db.execute("INSERT INTO audit(time,action,detail) VALUES(?,?,?)" , (nowtext() , action , detail))

    def ingest(self , rows , source , synced_day = None):
        added = 0
        with self.connect() as db:
            for row in rows:
                day = row["time"][:10]
                old = db.execute("SELECT time,name,status FROM records WHERE day=? AND uid=?" , (day , row["uid"])).fetchone()
                if old and tuple(old) != (row["time"] , row["name"] , row["status"]):
                    raise Problem(f"{day} 的卡片 {row['uid']} 与已保存记录冲突，整批未导入" , 409)
                if old:
                    continue
                db.execute("INSERT INTO records VALUES(?,?,?,?,?,?,?)" ,
                           (day , row["uid"] , row["time"] , row["name"] , row["status"] , source , nowtext()))
                db.execute("INSERT OR IGNORE INTO members(uid,name,updated_at) VALUES(?,?,?)" ,
                           (row["uid"] , row["name"] , nowtext()))
                added += 1
                if not synced_day:
                    db.execute("DELETE FROM syncs WHERE day=?" , (day ,))
            if synced_day:
                db.execute("INSERT INTO syncs VALUES(?,?,?) ON CONFLICT(day) DO UPDATE SET synced_at=excluded.synced_at,count=excluded.count" ,
                           (synced_day , nowtext() , len(rows)))
            db.execute("INSERT INTO audit(time,action,detail) VALUES(?,?,?)" ,
                       (nowtext() , "同步记录" if synced_day else "导入记录" , f"{source}，新增 {added}，已有 {len(rows) - added}"))
        return {"added": added , "existing": len(rows) - added , "total": len(rows)}

    def roster(self , members):
        with self.connect() as db:
            db.execute("UPDATE members SET on_device=0")
            for uid , name in members:
                db.execute("INSERT INTO members(uid,name,on_device,updated_at) VALUES(?,?,1,?) "
                           "ON CONFLICT(uid) DO UPDATE SET name=excluded.name,on_device=1,updated_at=excluded.updated_at" ,
                           (uid , name , nowtext()))

    def members(self , search = ""):
        with self.connect() as db:
            rows = db.execute("SELECT m.*, (SELECT MAX(time) FROM records r WHERE r.uid=m.uid) AS last_checkin "
                              "FROM members m ORDER BY on_device DESC,name,uid").fetchall()
        return [dict(row) for row in rows if not search or search.casefold() in " ".join(str(v) for v in row).casefold()]

    def edit(self , uid , data):
        uid = validuid(uid)
        values = (field(data.get("student_no" , "") , 40) , field(data.get("group_name" , "") , 40) ,
                  field(data.get("notes" , "") , 200) , nowtext() , uid)
        with self.connect() as db:
            if not db.execute("UPDATE members SET student_no=?,group_name=?,notes=?,updated_at=? WHERE uid=?" , values).rowcount:
                raise Problem("卡片尚未登记或导入" , 404)
        self.audit("修改人员备注" , uid)

    def records(self , start , end , search = ""):
        validdate(start)
        validdate(end)
        if start > end:
            raise Problem("开始日期不能晚于结束日期")
        with self.connect() as db:
            rows = db.execute("SELECT r.*,COALESCE(m.student_no,'') AS student_no,COALESCE(m.group_name,'') AS group_name "
                              "FROM records r LEFT JOIN members m ON r.uid=m.uid WHERE r.day BETWEEN ? AND ? ORDER BY r.time DESC,r.uid" ,
                              (start , end)).fetchall()
        return [dict(row) for row in rows if not search or search.casefold() in " ".join(str(v) for v in row).casefold()]

    def overview(self , day):
        validdate(day)
        start = (datetime.strptime(day , "%Y-%m-%d") - timedelta(days = 6)).strftime("%Y-%m-%d")
        records = self.records(day , day)
        members = self.members()
        checked = {row["uid"] for row in records}
        with self.connect() as db:
            counts = dict(db.execute("SELECT day,COUNT(*) FROM records WHERE day BETWEEN ? AND ? GROUP BY day" , (start , day)).fetchall())
            sync = db.execute("SELECT * FROM syncs WHERE day=?" , (day ,)).fetchone()
            total = db.execute("SELECT COUNT(*) FROM records").fetchone()[0]
            audit = [dict(row) for row in db.execute("SELECT * FROM audit ORDER BY id DESC LIMIT 12")]
        trend = []
        for i in range(7):
            date = (datetime.strptime(start , "%Y-%m-%d") + timedelta(days = i)).strftime("%Y-%m-%d")
            trend.append({"day": date , "count": counts.get(date , 0)})
        return {"day": day , "count": len(records) , "members": len(members) , "registered": sum(m["on_device"] for m in members) ,
                "total": total , "records": records[:8] , "trend": trend , "sync": dict(sync) if sync else None ,
                "unseen": [m for m in members if m["on_device"] and m["uid"] not in checked] , "audit": audit}

    def export(self , start , end , search = ""):
        out = io.StringIO(newline = "")
        writer = csv.writer(out , lineterminator = "\n")
        writer.writerow(["time" , "uid" , "name" , "status" , "student_no" , "group_name"])
        for row in self.records(start , end , search):
            writer.writerow([safecell(row[key]) for key in ("time" , "uid" , "name" , "status" , "student_no" , "group_name")])
        return out.getvalue().encode("utf-8-sig")

    def backup(self , target):
        with self.connect() as source , closing(sqlite3.connect(target)) as dest:
            source.backup(dest)
