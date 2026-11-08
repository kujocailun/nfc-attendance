#include <cassert>
#include <cstdio>
#include <Arduino.h>
#include <SD.h>
#include "../../src/attendance.cpp"
using namespace std;

map<string , vector<uint8_t>> host_files;
set<string> host_dirs;
bool host_fail_open , host_short_write , host_short_read , host_fail_seek;
bool host_fail_commit , host_corrupt_readback , host_fail_mkdir;
signed host_handles , host_write_opens , host_feedback;
bool host_storage = true , host_clock = true;
uint32_t host_now = 10000;
string host_stamp = "2026-11-07 09:00:00";
SDFS SD;
Stream output;
string before , first_day;
char generated_uid[21];
const char uid1[] = "04-11-22-33";
const char uid2[] = "04-01-02-03-04-05-06";
const char day1[] = "/attendance/2026-11-07.csv";
const char day2[] = "/attendance/2026-11-08.csv";

uint32_t millis()
{
    return host_now;
}

bool storageready()
{
    return host_storage;
}

bool clockvalid()
{
    return host_clock;
}

const char *clocktext()
{
    return host_stamp.c_str();
}

void cardfeedback(bool success)
{
    host_feedback = success ? 1 : 2;
}

void duplicatefeedback()
{
    host_feedback = 4;
}

string contents(const char *path)
{
    auto &bytes = host_files.at(path);
    return string(bytes.begin() , bytes.end());
}

void putfile(const char *path , const string &value)
{
    host_files[path] = vector<uint8_t>(value.begin() , value.end());
}

void reset()
{
    assert(host_handles == 0);
    host_files.clear();
    host_dirs = {"/"};
    host_fail_open = host_short_write = host_short_read = host_fail_seek = false;
    host_fail_commit = host_corrupt_readback = host_fail_mkdir = false;
    host_storage = host_clock = true;
    host_now = 10000;
    host_stamp = "2026-11-07 09:00:00";
    host_write_opens = host_feedback = 0;
    memset(attendance_active , 0 , sizeof(attendance_active));
    memset(attendance_bad , 0 , sizeof(attendance_bad));
    event_uid[0] = event_name[0] = event_time[0] = 0;
    output.text.clear();
    initattendance();
}

void command(const string &value , uint8_t port = 0)
{
    for (unsigned char c : value)
    {
        assert(attendanceinput(output , port , c));
    }
}

void enroll()
{
    command("N04-11-22-33,student\n");
    assert(member_count == 1 && host_feedback == 1);
}

void sol()
{
    reset();
    assert(members_ready && member_count == 0 && today_count == 0 && host_files.empty());
    checkin(uid1 , output);
    assert(string(attendance_state) == "UNKNOWN_CARD" && host_feedback == 2 && host_files.empty());
    enroll();
    assert(contents(members_path) == "uid,name\n04-11-22-33,student\n");
    before = contents(members_path);
    command("N04-11-22-33,student\r\n");
    command("N04-11-22-33,other\n");
    assert(contents(members_path) == before && member_count == 1);
    assert(output.text.find("ALREADY_BOUND") != string::npos && output.text.find("UID_CONFLICT") != string::npos);
    checkin(uid1 , output);
    assert(string(attendance_state) == "SAVED" && today_count == 1 && host_feedback == 1);
    first_day = "time,uid,name,status\n2026-11-07 09:00:00,04-11-22-33,student,SAVED\n";
    assert(contents(day1) == first_day);
    checkin(uid1 , output);
    assert(string(attendance_state) == "ALREADY_TODAY" && host_feedback == 4 && contents(day1) == first_day);
    member_count = today_count = 0;
    members_ready = false;
    initattendance();
    assert(members_ready && member_count == 1 && today_count == 1);
    checkin(uid1 , output);
    assert(string(attendance_state) == "ALREADY_TODAY" && contents(day1) == first_day);

    command("N04-01-02-03-04-05-06,学生甲\n");
    checkin(uid2 , output);
    assert(string(attendance_state) == "SAVED" && today_count == 2);
    before = contents(day1);
    output.text.clear();
    command("E2026-11-07\n");
    assert(output.text.find(before) != string::npos && output.text.find("[EXPORT-END] result=PASS") != string::npos);
    assert(contents(day1) == before && host_handles == 0);

    host_stamp = "2026-11-08 00:00:01";
    host_now += 1000;
    pollattendance();
    assert(today_count == 0);
    checkin(uid1 , output);
    assert(string(attendance_state) == "SAVED" && today_count == 1 && SD.exists(day2));
    assert(contents(day1) == before);
    output.text.clear();
    command("E2026-11-07\n");
    assert(today_count == 1 && output.text.find(before) != string::npos);

    host_clock = false;
    before = contents(day2);
    checkin(uid1 , output);
    assert(string(attendance_state) == "CLOCK_ERROR" && host_feedback == 2 && contents(day2) == before);
    host_clock = true;
    host_stamp = "2026-02-30 09:00:00";
    checkin(uid1 , output);
    assert(string(attendance_state) == "CLOCK_ERROR");
    assert(validdate("2028-02-29") && !validdate("2026-02-29") && !validdate("2026-13-01"));
    assert(!validstamp("2026-11-07 24:00:00") && !validstamp("2026-11-07 09:60:00"));
    host_storage = false;
    checkin(uid1 , output);
    assert(string(attendance_state) == "SD_OFFLINE" && host_feedback == 2);

    reset();
    enroll();
    host_short_write = true;
    checkin(uid1 , output);
    assert(string(attendance_state) == "WRITE_FAILED" && host_feedback == 2 && today_count == 0);
    host_short_write = false;
    before = contents(day1);
    checkin(uid1 , output);
    assert(string(attendance_state) == "LOG_ERROR" && contents(day1) == before);
    initattendance();
    assert(string(attendance_state) == "LOG_ERROR");

    reset();
    enroll();
    host_fail_commit = true;
    checkin(uid1 , output);
    assert(string(attendance_state) == "WRITE_FAILED" && host_feedback == 2 && today_count == 0);

    reset();
    enroll();
    host_corrupt_readback = true;
    checkin(uid1 , output);
    assert(string(attendance_state) == "WRITE_FAILED" && host_feedback == 2);
    host_corrupt_readback = false;
    before = contents(day1);
    checkin(uid1 , output);
    assert(string(attendance_state) == "ALREADY_TODAY" && contents(day1) == before);

    reset();
    enroll();
    checkin(uid1 , output);
    before = contents(day1);
    putfile(day1 , before + "partial");
    checkin(uid1 , output);
    assert(string(attendance_state) == "LOG_ERROR" && contents(day1) == before + "partial");
    putfile(day1 , before + before.substr(before.find('\n') + 1));
    checkin(uid1 , output);
    assert(string(attendance_state) == "LOG_ERROR");

    reset();
    enroll();
    checkin(uid1 , output);
    before = contents(day1);
    host_short_read = true;
    checkin(uid1 , output);
    assert(string(attendance_state) == "LOG_ERROR" && host_feedback == 2);
    assert(contents(day1) == before && host_handles == 0);
    host_short_read = false;
    host_fail_open = true;
    checkin(uid1 , output);
    assert(string(attendance_state) == "LOG_ERROR" && contents(day1) == before && host_handles == 0);

    reset();
    enroll();
    host_fail_open = true;
    checkin(uid1 , output);
    assert(string(attendance_state) == "WRITE_FAILED" && host_feedback == 2 && !SD.exists(day1));

    reset();
    host_short_write = true;
    command("N04-11-22-33,student\n");
    assert(!members_ready && member_count == 0 && host_feedback == 2);
    host_short_write = false;
    before = contents(members_path);
    initattendance();
    assert(!members_ready && contents(members_path) == before);
    checkin(uid1 , output);
    assert(string(attendance_state) == "REGISTRY_ERROR" && !SD.exists(day1));

    reset();
    putfile(members_path , "uid,name\n04-11-22-33,student\n04-11-22-33,other\n");
    initattendance();
    assert(!members_ready);
    reset();
    putfile(members_path , "wrong header\n");
    initattendance();
    assert(!members_ready);

    reset();
    enroll();
    host_fail_mkdir = true;
    checkin(uid1 , output);
    assert(string(attendance_state) == "WRITE_FAILED" && !SD.exists(day1));
    host_fail_mkdir = false;
    host_fail_seek = true;
    checkin(uid1 , output);
    assert(string(attendance_state) == "WRITE_FAILED" && host_handles == 0);

    reset();
    command("N04-11-22-33,=formula\n");
    command("N04-11-22-33,a,b\n");
    command("NFA-AF-D8,short\n");
    command("N04-11-22-33,\n");
    command("N" + string(150 , 'x') + "\n");
    assert(member_count == 0 && host_files.empty());
    assert(!attendanceinput(output , 0 , 'r'));
    command("NFA-AF");
    host_now += 6000;
    assert(!attendanceinput(output , 0 , '\n'));
    command("N04-11-22-33,Test");
    command("N04-01-02-03-04-05-06,Other\n" , 1);
    command("\n");
    assert(member_count == 2 && string(members[1].name) == "Test");
    output.text.clear();
    command("E../../bad\n");
    command("E2026-02-30\n");
    assert(output.text.find("[EXPORT-BEGIN]") == string::npos);

    reset();
    for (int i = 0 ; i < member_capacity ; i ++)
    {
        snprintf(generated_uid , sizeof(generated_uid) , "%02X-00-00-00" , static_cast<unsigned>(i));
        bindmember(generated_uid , "member" , output);
    }
    assert(member_count == member_capacity);
    before = contents(members_path);
    bindmember("FF-FF-FF-FF" , "extra" , output);
    assert(member_count == member_capacity && contents(members_path) == before);
    initattendance();
    assert(members_ready && member_count == member_capacity && host_handles == 0);
    reportattendance(output);
    listmembers(output);
    assert(output.text.find("ONE_PER_UID_PER_DAY") != string::npos);
    reset();
    assert(attendancebind(uid1 , "student" , output));
    before = contents(members_path);
    assert(attendancebind(uid1 , "student" , output));
    assert(contents(members_path) == before);
    assert(!attendancebind(uid1 , "changed" , output));
    assert(contents(members_path) == before);
    char copied_uid[21] , copied_name[49] , copied_time[20];
    assert(attendancemember(0 , copied_uid , copied_name));
    assert(string(copied_uid) == uid1 && string(copied_name) == "student");
    assert(!attendancemember(1 , copied_uid , copied_name));
    checkin(uid1 , output);
    assert(attendanceday("2026-11-07"));
    char row[] = "2026-11-07 09:00:00,04-11-22-33,student,SAVED";
    assert(attendancerecord(row , copied_time , copied_uid , copied_name));
    assert(string(copied_time) == "2026-11-07 09:00:00");
    char invalid_row[] = "2026-11-07 25:00:00,04-11-22-33,student,SAVED";
    assert(!attendancerecord(invalid_row , copied_time , copied_uid , copied_name));
    host_corrupt_readback = true;
    assert(!attendancebind(uid2 , "new member" , output));
    assert(!members_ready && host_handles == 0);
    puts("PASS: original attendance regression plus remote registration readback, idempotent command retry, immutable names, roster bounds, history validation and damaged-write rejection.");
}

signed main ()
{
    sol();
    return 0;
}
