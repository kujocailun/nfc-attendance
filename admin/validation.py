import csv
import io
import re
from datetime import datetime , timedelta , timezone

CHINA = timezone(timedelta(hours = 8))
HEADER = ["time" , "uid" , "name" , "status"]


class Problem(Exception):
    def __init__(self , message , status = 400):
        super().__init__(message)
        self.status = status


def nowtext():
    return datetime.now(CHINA).strftime("%Y-%m-%d %H:%M:%S")


def today():
    return nowtext()[:10]


def validdate(value):
    if not isinstance(value , str) or not re.fullmatch(r"20\d{2}-\d{2}-\d{2}" , value):
        raise Problem("日期格式应为 YYYY-MM-DD，年份范围 2000～2099")
    try:
        datetime.strptime(value , "%Y-%m-%d")
    except ValueError:
        raise Problem("日期不存在") from None
    return value


def validuid(value):
    if not isinstance(value , str):
        raise Problem("请填写卡片 UID")
    value = value.strip().upper()
    if not re.fullmatch(r"[0-9A-F]{2}(?:-[0-9A-F]{2}){3}(?:(?:-[0-9A-F]{2}){3})?" , value):
        raise Problem("UID 应为 4 或 7 字节，例如 04-11-22-33")
    return value


def validname(value):
    if not isinstance(value , str) or not value.strip() or len(value.encode("utf-8")) > 48:
        raise Problem("姓名不能为空，且不能超过 48 个 UTF-8 字节（通常为 16 个汉字）")
    if value.lstrip().startswith(("=" , "+" , "-" , "@")) or any(ord(c) < 32 or ord(c) == 127 or c in ',"' for c in value):
        raise Problem("姓名不能含逗号、双引号、控制字符或以公式符号开头")
    return value


def field(value , limit):
    if not isinstance(value , str) or len(value) > limit or any(ord(c) < 32 for c in value):
        raise Problem(f"文字不能超过 {limit} 字，且不能含控制字符")
    return value.strip()


def readcsv(raw , expected_date = None):
    if not raw or len(raw) > 2 * 1024 * 1024:
        raise Problem("CSV 为空或超过 2 MiB")
    try:
        text = raw.decode("utf-8-sig")
        if not text.endswith("\n"):
            raise Problem("CSV 最后一行不完整，请重新导出")
        rows = list(csv.reader(io.StringIO(text , newline = "") , strict = True))
    except (UnicodeDecodeError , csv.Error):
        raise Problem("请使用终端导出的 UTF-8 CSV 文件") from None
    if not rows or rows[0] != HEADER:
        raise Problem("CSV 表头必须为 time,uid,name,status")
    result = []
    seen = set()
    for index , row in enumerate(rows[1:] , 2):
        if len(row) != 4:
            raise Problem(f"第 {index} 行应有四列")
        stamp , uid , name , state = row
        if not re.fullmatch(r"20\d{2}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}" , stamp):
            raise Problem(f"第 {index} 行时间格式错误")
        try:
            datetime.strptime(stamp , "%Y-%m-%d %H:%M:%S")
        except ValueError:
            raise Problem(f"第 {index} 行时间不存在") from None
        uid = validuid(uid)
        validname(name)
        if state != "SAVED" or (expected_date and stamp[:10] != expected_date):
            raise Problem(f"第 {index} 行状态或日期与导出请求不符")
        key = (stamp[:10] , uid)
        if key in seen:
            raise Problem(f"第 {index} 行同一卡片同一天重复出现")
        seen.add(key)
        result.append({"time": stamp , "uid": uid , "name": name , "status": state})
    return result


def safecell(value):
    text = str(value)
    return "'" + text if text.lstrip().startswith(("=" , "+" , "-" , "@")) else text
