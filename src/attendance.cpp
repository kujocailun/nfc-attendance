#include <Arduino.h>
#include <SD.h>
#include <cstring>
#include <cstdio>
#include "attendance.h"
#include "storage.h"
#include "clock.h"
#include "feedback.h"
using namespace std;
#define int long long

const uint16_t member_capacity = 128;
const char members_path[] = "/members.csv";
const char members_header[] = "uid,name";
const char attendance_header[] = "time,uid,name,status";
const uint8_t month_days[] = {0 , 31 , 28 , 31 , 30 , 31 , 30 , 31 , 31 , 30 , 31 , 30 , 31};
struct member_entry
{
    char uid[21] , name[49];
};
member_entry members[member_capacity];
uint16_t member_count , today_count , scan_count;
bool members_ready , scan_seen[member_capacity] , scan_found;
const char *attendance_state = "NOT_INITIALIZED";
const char *attendance_issue = "NONE";
char event_uid[21] , event_name[49] , event_time[20];
char today_date[11] , scan_date[11] , scan_path[40] , canonical_uid[21];
char working_date[11] , validation_date[11];
char attendance_line[192] , attendance_packet[256] , attendance_verify[256];
char attendance_cmd[2][100];
uint8_t attendance_len[2];
bool attendance_active[2] , attendance_bad[2];
uint32_t attendance_input_at[2] , attendance_changed;
uint32_t day_poll_at;
bool day_poll_started;
size_t attendance_expected;

bool validuid(const char *value)
{
    size_t len = strlen(value);
    if (len != 11 && len != 20)
    {
        return false;
    }
    for (size_t i = 0 ; i < len ; i ++)
    {
        char c = value[i];
        if (i % 3 == 2)
        {
            if (c != '-')
            {
                return false;
            }
        }
        else
        {
            if (c >= 'a' && c <= 'f')
            {
                c -= 'a' - 'A';
            }
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            {
                return false;
            }
        }
        canonical_uid[i] = c;
    }
    canonical_uid[len] = 0;
    return true;
}

bool validname(const char *value)
{
    size_t len = strlen(value);
    if (!len || len > 48)
    {
        return false;
    }
    bool started = false;
    for (size_t i = 0 ; i < len ; i ++)
    {
        uint8_t c = static_cast<uint8_t>(value[i]);
        if (c < 32 || c == 127 || c == ',' || c == '"')
        {
            return false;
        }
        if (!started && c != ' ')
        {
            if (c == '=' || c == '+' || c == '-' || c == '@')
            {
                return false;
            }
            started = true;
        }
    }
    return started;
}

bool validdate(const char *value)
{
    if (strlen(value) != 10 || value[4] != '-' || value[7] != '-')
    {
        return false;
    }
    for (int i = 0 ; i < 10 ; i ++)
    {
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9'))
        {
            return false;
        }
    }
    unsigned y = (value[0] - '0') * 1000 + (value[1] - '0') * 100 + (value[2] - '0') * 10 + value[3] - '0';
    unsigned m = (value[5] - '0') * 10 + value[6] - '0';
    unsigned d = (value[8] - '0') * 10 + value[9] - '0';
    return y >= 2000 && y <= 2099 && m >= 1 && m <= 12 && d >= 1 &&
           d <= static_cast<unsigned>(month_days[m] + (m == 2 && y % 4 == 0));
}

bool validstamp(const char *value)
{
    if (strlen(value) != 19 || value[10] != ' ' || value[13] != ':' || value[16] != ':')
    {
        return false;
    }
    memcpy(validation_date , value , 10);
    validation_date[10] = 0;
    if (!validdate(validation_date))
    {
        return false;
    }
    for (int i = 11 ; i < 19 ; i ++)
    {
        if (i != 13 && i != 16 && (value[i] < '0' || value[i] > '9'))
        {
            return false;
        }
    }
    return (value[11] - '0') * 10 + value[12] - '0' < 24 &&
           (value[14] - '0') * 10 + value[15] - '0' < 60 &&
           (value[17] - '0') * 10 + value[18] - '0' < 60;
}

signed findmember(const char *uid)
{
    for (int i = 0 ; i < member_count ; i ++)
    {
        if (!strcmp(members[i].uid , uid))
        {
            return static_cast<signed>(i);
        }
    }
    return -1;
}

signed attendanceline(File &file)
{
    if (file.position() == file.size())
    {
        return 0;
    }
    size_t len = 0;
    while (len + 1 < sizeof(attendance_line))
    {
        signed c = file.read();
        if (c < 0 || c == 0)
        {
            return -1;
        }
        if (c == '\n')
        {
            if (len && attendance_line[len - 1] == '\r')
            {
                len --;
            }
            attendance_line[len] = 0;
            return 1;
        }
        attendance_line[len ++] = static_cast<char>(c);
    }
    return -1;
}

bool readfailure(File &file , const char *issue)
{
    file.close();
    attendance_issue = issue;
    return false;
}

bool loadmembers()
{
    member_count = 0;
    if (!SD.exists(members_path))
    {
        return true;
    }
    File file = SD.open(members_path , FILE_READ);
    if (!file || file.isDirectory() || file.size() > 16384 || attendanceline(file) != 1 ||
        strcmp(attendance_line , members_header))
    {
        return readfailure(file , "MEMBERS_HEADER_OR_OPEN");
    }
    signed result;
    while ((result = attendanceline(file)) == 1)
    {
        char *comma = strchr(attendance_line , ',');
        if (!comma)
        {
            return readfailure(file , "MEMBERS_ROW");
        }
        *comma = 0;
        if (member_count >= member_capacity || !validuid(attendance_line) || !validname(comma + 1) ||
            findmember(canonical_uid) >= 0)
        {
            return readfailure(file , "MEMBERS_ROW_OR_DUPLICATE");
        }
        strcpy(members[member_count].uid , canonical_uid);
        strcpy(members[member_count].name , comma + 1);
        member_count ++;
    }
    file.close();
    if (result < 0)
    {
        attendance_issue = "MEMBERS_INCOMPLETE_ROW";
        return false;
    }
    return true;
}

bool scanday(const char *date , const char *uid)
{
    strcpy(scan_date , date);
    snprintf(scan_path , sizeof(scan_path) , "/attendance/%s.csv" , scan_date);
    scan_count = 0;
    scan_found = false;
    attendance_expected = 0;
    memset(scan_seen , 0 , sizeof(scan_seen));
    if (!SD.exists(scan_path))
    {
        return true;
    }
    File file = SD.open(scan_path , FILE_READ);
    if (!file || file.isDirectory() || file.size() > 32768 || attendanceline(file) != 1 ||
        strcmp(attendance_line , attendance_header))
    {
        return readfailure(file , "LOG_HEADER_OR_OPEN");
    }
    attendance_expected = file.size();
    signed result;
    while ((result = attendanceline(file)) == 1)
    {
        char *a = strchr(attendance_line , ',');
        char *b = a ? strchr(a + 1 , ',') : nullptr;
        char *c = b ? strchr(b + 1 , ',') : nullptr;
        if (!a || !b || !c)
        {
            return readfailure(file , "LOG_ROW");
        }
        *a = *b = *c = 0;
        if (!validstamp(attendance_line) || strncmp(attendance_line , scan_date , 10) ||
            !validuid(a + 1) || strcmp(c + 1 , "SAVED"))
        {
            return readfailure(file , "LOG_FIELDS");
        }
        signed index = findmember(canonical_uid);
        if (index < 0 || scan_seen[index] || strcmp(b + 1 , members[index].name))
        {
            return readfailure(file , "LOG_MEMBER_OR_DUPLICATE");
        }
        scan_seen[index] = true;
        scan_count ++;
        scan_found |= uid && !strcmp(uid , canonical_uid);
    }
    file.close();
    if (result < 0)
    {
        attendance_issue = "LOG_INCOMPLETE_ROW";
        return false;
    }
    return true;
}

bool appendverified(const char *path)
{
    size_t len = strlen(attendance_packet);
    File file = SD.open(path , FILE_APPEND);
    if (!file || file.isDirectory() || !file.seek(0 , SeekEnd) || file.position() != attendance_expected)
    {
        return readfailure(file , "APPEND_OPEN_OR_CONFLICT");
    }
    size_t written = file.write(reinterpret_cast<const uint8_t *>(attendance_packet) , len);
    file.flush();
    file.close();
    if (written != len)
    {
        attendance_issue = "WRITE_SHORT";
        return false;
    }
    file = SD.open(path , FILE_READ);
    if (!file || file.size() != attendance_expected + len || !file.seek(attendance_expected , SeekSet) ||
        file.read(reinterpret_cast<uint8_t *>(attendance_verify) , len) != len ||
        memcmp(attendance_verify , attendance_packet , len))
    {
        return readfailure(file , "WRITE_READBACK_FAILED");
    }
    file.close();
    return true;
}

void setattendance(const char *state)
{
    attendance_state = state;
    attendance_changed = millis();
}

void reportattendanceevent(Stream &out)
{
    out.printf("[ATTENDANCE] state=%s time=%s uid=%s name=%s detail=%s\n" ,
               attendance_state , event_time , event_uid , event_name , attendance_issue);
}

void pollattendance()
{
    uint32_t now = millis();
    if (day_poll_started && now - day_poll_at < 1000)
    {
        return;
    }
    day_poll_started = true;
    day_poll_at = now;
    if (!members_ready || !storageready() || !clockvalid() || !validstamp(clocktext()) ||
        !strncmp(today_date , clocktext() , 10))
    {
        return;
    }
    memcpy(working_date , clocktext() , 10);
    working_date[10] = 0;
    if (!scanday(working_date , nullptr))
    {
        setattendance("LOG_ERROR");
        return;
    }
    strcpy(today_date , working_date);
    today_count = scan_count;
}

void initattendance()
{
    attendance_issue = "NONE";
    members_ready = false;
    today_date[0] = 0;
    today_count = 0;
    day_poll_started = false;
    if (!storageready())
    {
        setattendance("SD_OFFLINE");
        return;
    }
    members_ready = loadmembers();
    setattendance(members_ready ? "READY" : "REGISTRY_ERROR");
    pollattendance();
}

void bindmember(const char *uid , const char *name , Stream &out)
{
    attendance_issue = "NONE";
    if (!storageready() || !members_ready)
    {
        out.print("[BIND] STORAGE_OR_REGISTRY_UNAVAILABLE\n");
        return;
    }
    if (!validuid(uid) || !validname(name))
    {
        out.print("[BIND] INVALID_UID_OR_NAME\n");
        return;
    }
    signed index = findmember(canonical_uid);
    if (index >= 0)
    {
        out.printf("[BIND] %s uid=%s name=%s\n" ,
                   !strcmp(name , members[index].name) ? "ALREADY_BOUND" : "UID_CONFLICT" ,
                   members[index].uid , members[index].name);
        return;
    }
    if (member_count >= member_capacity)
    {
        out.print("[BIND] MEMBER_LIMIT\n");
        return;
    }
    attendance_expected = 0;
    bool existing = SD.exists(members_path);
    if (existing)
    {
        File file = SD.open(members_path , FILE_READ);
        if (!file || file.isDirectory())
        {
            readfailure(file , "MEMBERS_READ_OPEN");
            out.print("[BIND] MEMBERS_READ_OPEN\n");
            return;
        }
        attendance_expected = file.size();
        file.close();
    }
    snprintf(attendance_packet , sizeof(attendance_packet) , "%s%s%s,%s\n" ,
             existing ? "" : members_header , existing ? "" : "\n" , canonical_uid , name);
    if (!appendverified(members_path))
    {
        members_ready = false;
        setattendance("REGISTRY_ERROR");
        out.printf("[BIND] WRITE_FAILED detail=%s\n" , attendance_issue);
        cardfeedback(false);
        return;
    }
    strcpy(members[member_count].uid , canonical_uid);
    strcpy(members[member_count].name , name);
    member_count ++;
    strcpy(event_uid , canonical_uid);
    strcpy(event_name , name);
    snprintf(event_time , sizeof(event_time) , "%s" , clockvalid() ? clocktext() : "CLOCK INVALID");
    setattendance("ENROLLED");
    out.printf("[BIND] SAVED uid=%s name=%s members=%u\n" , event_uid , event_name , static_cast<unsigned>(member_count));
    cardfeedback(true);
}

void checkin(const char *uid , Stream &out)
{
    attendance_issue = "NONE";
    snprintf(event_uid , sizeof(event_uid) , "%s" , uid);
    event_name[0] = 0;
    snprintf(event_time , sizeof(event_time) , "%s" , clockvalid() ? clocktext() : "CLOCK INVALID");
    const char *result = nullptr;
    signed index = validuid(uid) ? findmember(canonical_uid) : -1;
    if (index >= 0)
    {
        strcpy(event_name , members[index].name);
    }
    if (!storageready())
    {
        result = "SD_OFFLINE";
    }
    else if (!members_ready)
    {
        result = "REGISTRY_ERROR";
    }
    else if (index < 0)
    {
        result = "UNKNOWN_CARD";
    }
    else if (!clockvalid() || !validstamp(clocktext()))
    {
        result = "CLOCK_ERROR";
    }
    else
    {
        memcpy(working_date , event_time , 10);
        working_date[10] = 0;
        if (!scanday(working_date , members[index].uid))
        {
            result = "LOG_ERROR";
        }
        else if (scan_found)
        {
            strcpy(today_date , working_date);
            today_count = scan_count;
            result = "ALREADY_TODAY";
        }
        else
        {
            bool folder_ok = SD.exists("/attendance");
            if (folder_ok)
            {
                File folder = SD.open("/attendance" , FILE_READ);
                folder_ok = folder && folder.isDirectory();
                folder.close();
            }
            else
            {
                folder_ok = SD.mkdir("/attendance");
            }
            bool existing = SD.exists(scan_path);
            snprintf(attendance_packet , sizeof(attendance_packet) , "%s%s%s,%s,%s,SAVED\n" ,
                     existing ? "" : attendance_header , existing ? "" : "\n" ,
                     event_time , members[index].uid , event_name);
            if (!folder_ok)
            {
                attendance_issue = "ATTENDANCE_DIRECTORY";
                result = "WRITE_FAILED";
            }
            else if (!appendverified(scan_path))
            {
                result = "WRITE_FAILED";
            }
            else
            {
                strcpy(today_date , working_date);
                today_count = scan_count + 1;
                result = "SAVED";
            }
        }
    }
    setattendance(result);
    if (!strcmp(result , "ALREADY_TODAY"))
    {
        duplicatefeedback();
    }
    else
    {
        cardfeedback(!strcmp(result , "SAVED"));
    }
    reportattendanceevent(out);
}

void listmembers(Stream &out)
{
    out.printf("[MEMBERS] ready=%u count=%u limit=%u\n" , members_ready ,
               static_cast<unsigned>(member_count) , static_cast<unsigned>(member_capacity));
    if (members_ready)
    {
        for (int i = 0 ; i < member_count ; i ++)
        {
            out.printf("[MEMBER] uid=%s name=%s\n" , members[i].uid , members[i].name);
        }
    }
}

void exportattendance(const char *date , Stream &out)
{
    if (!validdate(date) || !storageready() || !members_ready)
    {
        out.print("[EXPORT] INVALID_DATE_OR_STORAGE\n");
        return;
    }
    if (!scanday(date , nullptr) || !SD.exists(scan_path))
    {
        out.printf("[EXPORT] NO_VALID_FILE detail=%s\n" , attendance_issue);
        return;
    }
    File file = SD.open(scan_path , FILE_READ);
    if (!file)
    {
        out.print("[EXPORT] READ_OPEN_FAILED\n");
        return;
    }
    out.printf("[EXPORT-BEGIN] date=%s bytes=%u\n" , date , static_cast<unsigned>(file.size()));
    size_t remaining = file.size();
    bool ok = true;
    while (remaining)
    {
        size_t count = remaining < sizeof(attendance_verify) ? remaining : sizeof(attendance_verify);
        if (file.read(reinterpret_cast<uint8_t *>(attendance_verify) , count) != count ||
            out.write(reinterpret_cast<uint8_t *>(attendance_verify) , count) != count)
        {
            ok = false;
            break;
        }
        remaining -= count;
    }
    file.close();
    out.printf("[EXPORT-END] result=%s\n" , ok ? "PASS" : "FAIL");
}

bool attendanceinput(Stream &out , uint8_t port , signed c)
{
    if (port >= 2)
    {
        return false;
    }
    uint32_t now = millis();
    if (attendance_active[port] && now - attendance_input_at[port] > 5000)
    {
        attendance_active[port] = false;
        out.print("[COMMAND] TIMEOUT\n");
    }
    if (!attendance_active[port])
    {
        if (c != 'N' && c != 'n' && c != 'E' && c != 'e')
        {
            return false;
        }
        attendance_active[port] = true;
        attendance_bad[port] = false;
        attendance_len[port] = 0;
    }
    attendance_input_at[port] = now;
    if (c == '\r')
    {
        return true;
    }
    if (c == '\n')
    {
        attendance_active[port] = false;
        attendance_cmd[port][attendance_len[port]] = 0;
        if (attendance_bad[port])
        {
            out.print("[COMMAND] TOO_LONG_OR_INVALID\n");
        }
        else if (attendance_cmd[port][0] == 'E' || attendance_cmd[port][0] == 'e')
        {
            exportattendance(attendance_cmd[port] + 1 , out);
        }
        else
        {
            char *comma = strchr(attendance_cmd[port] + 1 , ',');
            if (!comma)
            {
                out.print("[BIND] SYNTAX NUID,name followed by newline\n");
            }
            else
            {
                *comma = 0;
                bindmember(attendance_cmd[port] + 1 , comma + 1 , out);
            }
        }
        return true;
    }
    if (c <= 0 || static_cast<size_t>(attendance_len[port]) + 1 >= sizeof(attendance_cmd[port]))
    {
        attendance_bad[port] = true;
        return true;
    }
    attendance_cmd[port][attendance_len[port] ++] = static_cast<char>(c);
    return true;
}

void reportattendance(Stream &out)
{
    out.printf("attendance_state=%s registry_ready=%u members=%u daily_rule=ONE_PER_UID_PER_DAY\n" ,
               attendance_state , members_ready , static_cast<unsigned>(member_count));
    out.printf("attendance_today=%s count=%u detail=%s\n" , today_date , static_cast<unsigned>(today_count) , attendance_issue);
    out.print("Commands: NUID,name newline binds a card; l lists members; EYYYY-MM-DD newline exports daily CSV.\n");
}

bool attendancehealthy()
{
    return members_ready && storageready();
}

const char *attendancestate()
{
    return attendance_state;
}

const char *attendancename()
{
    return event_name;
}

uint32_t attendancechanged()
{
    return attendance_changed;
}

uint16_t attendancemembers()
{
    return member_count;
}

uint16_t attendancetoday()
{
    return today_count;
}

bool attendancemember(uint16_t index , char *uid , char *name)
{
    if (!members_ready || index >= member_count)
    {
        return false;
    }
    strcpy(uid , members[index].uid);
    strcpy(name , members[index].name);
    return true;
}

bool attendancebind(const char *uid , const char *name , Stream &out)
{
    bindmember(uid , name , out);
    //联网回执前重新打开名单校验，掉电后重试同一命令也安全。
    if (!storageready() || !validuid(uid) || !validname(name) || !loadmembers())
    {
        members_ready = false;
        return false;
    }
    signed index = findmember(uid);
    return members_ready && index >= 0 && !strcmp(members[index].name , name);
}

bool attendanceday(const char *date)
{
    return members_ready && storageready() && validdate(date) && scanday(date , nullptr);
}

bool attendancerecord(char *line , char *stamp , char *uid , char *name)
{
    char *a = strchr(line , ',');
    char *b = a ? strchr(a + 1 , ',') : nullptr;
    char *c = b ? strchr(b + 1 , ',') : nullptr;
    if (!a || !b || !c)
    {
        return false;
    }
    *a = *b = *c = 0;
    if (!validstamp(line) || !validuid(a + 1) || !validname(b + 1) || strcmp(c + 1 , "SAVED"))
    {
        return false;
    }
    strcpy(stamp , line);
    strcpy(uid , canonical_uid);
    strcpy(name , b + 1);
    return true;
}
