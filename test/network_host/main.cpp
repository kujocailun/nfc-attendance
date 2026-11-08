#define main attendance_regression_main
#include "../attendance_host/main.cpp"
#undef main
#include "json.h"

enum net_kind : uint8_t
{
    net_events = 1 , net_roster , net_heartbeat , net_commands , net_ack , net_time
};
struct host_job
{
    net_kind kind;
    char payload[20480];
};
struct host_reply
{
    char data[2048];
};
host_job net_outgoing;
host_reply net_result;
Stream &Serial = output;
void *net_jobs;
const signed pdTRUE = 1;
bool net_inflight , net_retry , net_command_acking;
uint32_t net_retry_at , net_pass_at , net_uploaded , net_heartbeat_at , net_commands_at , net_roster_at , net_time_at;
uint16_t net_seen_members , net_roster_sent_count;
File net_file , net_directory;
char net_id[16] = "s3-aabbccddeeff";
char net_command_id[33] , net_command_kind[12];
char net_line[192] , net_stamp[20] , net_uid[21] , net_name[49] , net_expected[65] , net_day[11];
bool host_setclock_ok = true;
signed host_setclock_calls;
string host_net_status;
const char task_id[] = "0123456789abcdef0123456789abcdef";

signed xQueueSend(void * , const void * , signed)
{
    return pdTRUE;
}

bool netdue(uint32_t now , uint32_t target)
{
    return static_cast<int32_t>(now - target) >= 0;
}

void netstatus(const char *value)
{
    host_net_status = value;
}

const char *jsontext(cJSON *object , const char *key)
{
    auto *value = cJSON_GetObjectItemCaseSensitive(object , key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

bool netserialize(cJSON *object)
{
    bool ok = cJSON_PrintPreallocated(object , net_outgoing.payload , sizeof(net_outgoing.payload) , false);
    cJSON_Delete(object);
    return ok;
}

signed mbedtls_sha256_ret(const uint8_t *data , size_t length , uint8_t *result , signed)
{
    std::memset(result , 0 , 32);
    for (size_t i = 0 ; i < length ; i ++)
    {
        result[i % 32] ^= data[i];
    }
    return 0;
}

bool clocksettext(const char * , Stream &)
{
    host_setclock_calls ++;
    return host_setclock_ok;
}

#include "../../src/network_sync.h"

void remote_reset()
{
    net_file.close();
    net_directory.close();
    reset();
    for (auto &entry : host_json)
    {
        cJSON_Delete(entry.second);
    }
    host_json.clear();
    net_inflight = net_retry = net_command_acking = false;
    net_command_id[0] = net_command_kind[0] = 0;
    net_pass_at = host_now;
    host_setclock_ok = true;
    host_setclock_calls = 0;
}

bool deliver(const char *kind , const char *id = task_id)
{
    auto *reply = cJSON_CreateObject();
    auto *array = cJSON_AddArrayToObject(reply , "commands");
    auto *item = cJSON_CreateObject();
    cJSON_AddItemToArray(array , item);
    cJSON_AddStringToObject(item , "id" , id);
    cJSON_AddStringToObject(item , "kind" , kind);
    cJSON_AddStringToObject(item , "uid" , uid1);
    cJSON_AddStringToObject(item , "name" , "student");
    bool ok = netcommand(reply);
    cJSON_Delete(reply);
    return ok;
}

bool receive(cJSON *reply)
{
    assert(cJSON_PrintPreallocated(reply , net_result.data , sizeof(net_result.data) , false));
    cJSON_Delete(reply);
    net_inflight = false;
    return nethandlereply();
}

void assert_ack(bool ok , const char *detail)
{
    assert(net_command_acking && net_outgoing.kind == net_ack && net_retry);
    auto *ack = cJSON_Parse(net_outgoing.payload);
    assert(string(jsontext(ack , "id")) == task_id);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ack , "ok")) == ok);
    assert(string(jsontext(ack , "detail")) == detail);
    cJSON_Delete(ack);
}

void confirm_ack()
{
    auto *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply , "acked" , task_id);
    assert(receive(reply));
    assert(!net_command_id[0] && !net_command_acking);
    net_retry = false;
}

void remote_sol()
{
    remote_reset();
    assert(!deliver("erase"));
    assert(!deliver("clock" , "bad"));
    assert(!net_command_id[0]);
    assert(deliver("clock"));
    assert(!net_command_acking && host_setclock_calls == 0);
    net_outgoing.kind = net_time;
    auto *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply , "time" , "2026-11-08 12:00:00");
    assert(receive(reply));
    assert(host_setclock_calls == 1);
    assert_ack(true , "RTC_READBACK_OK");
    confirm_ack();

    remote_reset();
    assert(deliver("clock"));
    host_setclock_ok = false;
    net_outgoing.kind = net_time;
    reply = cJSON_CreateObject();
    assert(receive(reply));
    assert_ack(false , "RTC_VERIFY_FAILED");

    remote_reset();
    enroll();
    assert(deliver("roster"));
    assert(!net_command_acking);
    assert(netbuildroster());
    auto *roster = cJSON_Parse(net_outgoing.payload);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(roster , "members")) == 1);
    cJSON_Delete(roster);
    reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply , "count" , 0);
    assert(!receive(reply));
    assert(!net_command_acking);
    reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply , "count" , 1);
    assert(receive(reply));
    assert_ack(true , "ROSTER_UPLOADED");
    confirm_ack();

    remote_reset();
    enroll();
    checkin(uid1 , output);
    string unchanged = contents(day1);
    assert(deliver("rescan"));
    assert(!net_command_acking);
    assert(netnextrecord() && net_outgoing.kind == net_events);
    assert(!net_command_acking);
    reply = cJSON_CreateObject();
    auto *acks = cJSON_AddArrayToObject(reply , "acked");
    cJSON_AddItemToArray(acks , cJSON_CreateString("wrong-event"));
    assert(!receive(reply));
    assert(!net_command_acking);
    reply = cJSON_CreateObject();
    acks = cJSON_AddArrayToObject(reply , "acked");
    cJSON_AddItemToArray(acks , cJSON_CreateString(net_expected));
    assert(receive(reply));
    assert(!netnextrecord());
    assert(!net_command_acking);
    assert(!netnextrecord());
    assert_ack(true , "HISTORY_SCAN_ACKED");
    assert(contents(day1) == unchanged);
    confirm_ack();
    assert(host_handles == 0);

    remote_reset();
    enroll();
    checkin(uid1 , output);
    putfile(day1 , contents(day1) + "broken");
    assert(deliver("rescan"));
    assert(!netnextrecord());
    assert_ack(false , "SD_HISTORY_INVALID");
    assert(host_handles == 0);

    remote_reset();
    assert(deliver("rescan"));
    assert(!netnextrecord());
    assert_ack(true , "HISTORY_SCAN_ACKED");

    remote_reset();
    assert(deliver("rescan"));
    host_fail_open = true;
    assert(!netnextrecord());
    assert_ack(false , "SD_OPEN_FAILED");

    remote_reset();
    assert(deliver("rescan"));
    host_storage = false;
    assert(!netnextrecord());
    assert_ack(false , "SD_OR_REGISTRY_ERROR");

    remote_reset();
    assert(deliver("bind"));
    assert_ack(true , "SD_READBACK_OK");
    confirm_ack();
    string members_before = contents(members_path);
    assert(deliver("bind"));
    assert_ack(true , "SD_READBACK_OK");
    assert(contents(members_path) == members_before && member_count == 1);
    remote_reset();
    puts("PASS: native remote command regression (10 scenarios): deferred readback, failed clock, roster ACK, history per-record ACK, damaged history, empty history, SD read failure, SD offline, invalid commands and idempotent registration.");
}

signed main ()
{
    sol();
    remote_sol();
    return 0;
}
