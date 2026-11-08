#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SD.h>
#include <cJSON.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>
#include "network.h"
#include "attendance.h"
#include "button.h"
#include "clock.h"
#include "storage.h"
#include "nfc.h"
#include "display.h"
#if __has_include("project_config.h")
#include "project_config.h"
#endif
#ifndef NFC_DEVICE_API_URL
#define NFC_DEVICE_API_URL "https://device-api.example.com/device/v1/"
#endif
using namespace std;
#define int long long

extern const uint8_t net_ca_start[] asm("_binary_certs_x509_crt_bundle_start");
const char net_url[] = NFC_DEVICE_API_URL;
const size_t net_payload_limit = 20480;
const size_t net_response_limit = 2048;
enum net_kind : uint8_t
{
    net_events = 1 , net_roster , net_heartbeat , net_commands , net_ack , net_time
};
struct net_config
{
    char ssid[33] , password[65] , secret[65] , code[17];
};
struct net_job
{
    net_kind kind;
    char payload[net_payload_limit];
};
struct net_reply
{
    bool ok;
    signed status;
    char data[net_response_limit];
};

net_config net_active , net_candidate;
net_job net_outgoing , net_worker_job;
net_reply net_result , net_worker_result;
QueueHandle_t net_jobs , net_results , net_controls;
TaskHandle_t net_task;
portMUX_TYPE net_lock = portMUX_INITIALIZER_UNLOCKED;
Preferences net_prefs;
WebServer net_server(80);
DNSServer net_dns;
char net_id[16] , net_state[32] = "STARTING";
char net_ap_ssid[24] , net_ap_password[17] , net_form_token[33];
bool net_ready , net_portal , net_inflight , net_retry , net_candidate_pending;
bool net_worker_ap , net_worker_using_candidate;
int32_t net_rssi;
uint32_t net_retry_at , net_heartbeat_at , net_commands_at , net_roster_at , net_time_at;
uint32_t net_pass_at , net_button_at , net_worker_connect_at , net_worker_try_at , net_ap_at;
uint32_t net_seen_change;
uint16_t net_seen_members = UINT16_MAX;
uint16_t net_roster_sent_count;
bool net_button_held , net_button_fired;
File net_directory , net_file;
char net_line[192] , net_stamp[20] , net_uid[21] , net_name[49] , net_expected[65];
char net_day[11];
uint32_t net_uploaded , net_failures;
char net_command_id[33] , net_command_kind[12];
bool net_command_acking;

const char *jsontext(cJSON *object , const char *key)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object , key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

bool netdue(uint32_t now , uint32_t target)
{
    return static_cast<int32_t>(now - target) >= 0;
}

void netstatus(const char *state)
{
    portENTER_CRITICAL(&net_lock);
    snprintf(net_state , sizeof(net_state) , "%s" , state);
    portEXIT_CRITICAL(&net_lock);
}

void netrandom(char *target , size_t bytes)
{
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0 ; i < bytes ; i ++)
    {
        uint8_t value = static_cast<uint8_t>(esp_random());
        target[i * 2] = hex[value >> 4];
        target[i * 2 + 1] = hex[value & 15];
    }
    target[bytes * 2] = 0;
}

bool netconfigread(const char *key , net_config &config)
{
    String raw = net_prefs.getString(key , "");
    cJSON *data = cJSON_Parse(raw.c_str());
    if (!data)
    {
        return false;
    }
    bool ok = strlen(jsontext(data , "ssid")) > 0 && strlen(jsontext(data , "ssid")) <= 32 &&
              strlen(jsontext(data , "password")) <= 64 && strlen(jsontext(data , "secret")) == 64 &&
              strlen(jsontext(data , "code")) <= 16;
    if (ok)
    {
        strcpy(config.ssid , jsontext(data , "ssid"));
        strcpy(config.password , jsontext(data , "password"));
        strcpy(config.secret , jsontext(data , "secret"));
        strcpy(config.code , jsontext(data , "code"));
    }
    cJSON_Delete(data);
    return ok;
}

bool netconfigsave(const char *key , const net_config &config)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data , "ssid" , config.ssid);
    cJSON_AddStringToObject(data , "password" , config.password);
    cJSON_AddStringToObject(data , "secret" , config.secret);
    cJSON_AddStringToObject(data , "code" , config.code);
    char *raw = cJSON_PrintUnformatted(data);
    bool ok = raw && net_prefs.putString(key , raw) == strlen(raw) && net_prefs.getString(key , "") == raw;
    cJSON_free(raw);
    cJSON_Delete(data);
    return ok;
}

class net_sink : public Stream
{
public:
    size_t used = 0;
    bool overflow = false;
    char *data;
    explicit net_sink(char *value) : data(value)
    {
        data[0] = 0;
    }
    size_t write(uint8_t c) override
    {
        if (used + 1 >= net_response_limit)
        {
            overflow = true;
            return 0;
        }
        data[used ++] = static_cast<char>(c);
        data[used] = 0;
        return 1;
    }
    signed available() override
    {
        return 0;
    }
    signed read() override
    {
        return -1;
    }
    signed peek() override
    {
        return -1;
    }
    void flush() override
    {
    }
};

bool netrequest(const char *path , const char *payload , const net_config &config , char *result , signed *status = nullptr)
{
    result[0] = 0;
    if (status)
    {
        *status = 0;
    }
    if (WiFi.status() != WL_CONNECTED || time(nullptr) < 1704067200)
    {
        netstatus(WiFi.status() == WL_CONNECTED ? "WAIT_TIME" : "HOTSPOT_OFFLINE");
        return false;
    }
    WiFiClientSecure client;
    client.setCACertBundle(net_ca_start);
    client.setHandshakeTimeout(8);
    client.setTimeout(8);
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client , String(net_url) + path))
    {
        netstatus("HTTPS_BEGIN_FAILED");
        return false;
    }
    const char *headers[] = {"Content-Type"};
    http.collectHeaders(headers , 1);
    http.addHeader("X-Device-ID" , net_id);
    http.addHeader("Authorization" , String("Bearer ") + config.secret);
    http.addHeader("Content-Type" , "application/json");
    signed code = payload ? http.POST(String(payload)) : http.GET();
    if (status)
    {
        *status = code;
    }
    net_sink sink(result);
    bool ok = code == 200 && http.header("Content-Type").startsWith("application/json") &&
              http.getSize() < static_cast<signed>(net_response_limit) && http.writeToStream(&sink) >= 0 && !sink.overflow;
    if (!ok)
    {
        char state[32];
        snprintf(state , sizeof(state) , "HTTPS_ERROR_%d" , code);
        netstatus(state);
        result[0] = 0;
    }
    http.end();
    return ok;
}

void netcloseportal()
{
    net_server.stop();
    net_dns.stop();
    WiFi.softAPdisconnect(true);
    net_worker_ap = false;
    portENTER_CRITICAL(&net_lock);
    net_portal = false;
    portEXIT_CRITICAL(&net_lock);
}

void netopenportal()
{
    if (net_worker_ap)
    {
        netcloseportal();
    }
    char password[17] , token[33] , ssid[24];
    netrandom(password , 6);
    netrandom(token , 16);
    snprintf(ssid , sizeof(ssid) , "SETUP-%s" , net_id + 9);
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(ssid , password , 1 , false , 1))
    {
        netstatus("AP_START_FAILED");
        return;
    }
    portENTER_CRITICAL(&net_lock);
    strcpy(net_ap_password , password);
    strcpy(net_form_token , token);
    strcpy(net_ap_ssid , ssid);
    net_portal = true;
    portEXIT_CRITICAL(&net_lock);
    net_dns.start(53 , "*" , WiFi.softAPIP());
    net_server.begin();
    net_worker_ap = true;
    net_ap_at = millis();
}

void netconnect(bool candidate)
{
    net_worker_using_candidate = candidate;
    net_config &config = candidate ? net_candidate : net_active;
    portENTER_CRITICAL(&net_lock);
    net_ready = false;
    portEXIT_CRITICAL(&net_lock);
    WiFi.disconnect();
    WiFi.begin(config.ssid , config.password);
    net_worker_connect_at = millis();
    net_worker_try_at = millis() + 3000;
    netstatus("CONNECTING_HOTSPOT");
}

void netportalroutes()
{
    net_server.on("/" , HTTP_GET , []()
    {
        String html = F("<!doctype html><html lang='zh-CN'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>签到终端配网</title><body><h2>签到终端配网</h2><p>安卓热点请使用 2.4GHz / WPA2 兼容模式。保存后断开此 Wi-Fi，再开启手机热点。</p><form method='post' action='/save'><label>热点名称<br><input name='ssid' maxlength='32' required></label><br><label>热点密码<br><input name='password' type='password' maxlength='63' minlength='8' required></label><br><label>后台一次性配对码<br><input name='code' maxlength='16' placeholder='已配对时仅换热点可留空'></label><input name='token' type='hidden' value='");
        html += net_form_token;
        html += F("'><br><button>保存并连接</button></form><p>首次配对前，请先在管理后台生成 10 分钟有效的配对码。</p></body></html>");
        net_server.sendHeader("Cache-Control" , "no-store");
        net_server.send(200 , "text/html; charset=utf-8" , html);
    });
    net_server.on("/save" , HTTP_POST , []()
    {
        String ssid = net_server.arg("ssid") , password = net_server.arg("password") , code = net_server.arg("code");
        code.trim();
        code.toUpperCase();
        bool codeok = code.length() == 16;
        for (size_t i = 0 ; i < code.length() ; i ++)
        {
            codeok &= (code[i] >= '0' && code[i] <= '9') || (code[i] >= 'A' && code[i] <= 'F');
        }
        if (net_server.arg("token") != net_form_token || !ssid.length() || ssid.length() > 32 ||
            password.length() < 8 || password.length() > 63 ||
            (!codeok && (code.length() || !net_active.ssid[0])))
        {
            net_server.send(400 , "text/plain; charset=utf-8" , "参数无效，请刷新后重填。首次配置需要 16 位配对码。");
            return;
        }
        net_config proposed = net_active;
        ssid.toCharArray(proposed.ssid , sizeof(proposed.ssid));
        password.toCharArray(proposed.password , sizeof(proposed.password));
        code.toCharArray(proposed.code , sizeof(proposed.code));
        if (codeok)
        {
            netrandom(proposed.secret , 32);
        }
        if (!netconfigsave("candidate" , proposed))
        {
            net_server.send(500 , "text/plain; charset=utf-8" , "配置保存失败，旧配置仍保留。");
            return;
        }
        net_candidate = proposed;
        net_candidate_pending = true;
        net_server.send(200 , "text/plain; charset=utf-8" , "候选配置已保存。请断开终端 Wi-Fi，开启安卓手机热点。终端连上后台后自动保存正式配置；失败时保留原配置。可关闭此页面。");
        netconnect(true);
    });
    net_server.onNotFound([]()
    {
        net_server.sendHeader("Location" , "http://192.168.4.1/");
        net_server.send(302 , "text/plain" , "Open http://192.168.4.1/");
    });
}

bool netpaircandidate()
{
    bool pairing = net_candidate.code[0] != 0;
    cJSON *object = cJSON_CreateObject();
    if (pairing)
    {
        cJSON_AddStringToObject(object , "code" , net_candidate.code);
        cJSON_AddStringToObject(object , "device_id" , net_id);
        cJSON_AddStringToObject(object , "secret" , net_candidate.secret);
    }
    char *payload = cJSON_PrintUnformatted(object);
    bool ok = payload && netrequest(pairing ? "pair" : "heartbeat" , payload , net_candidate , net_worker_result.data);
    cJSON_free(payload);
    cJSON_Delete(object);
    if (!ok)
    {
        return false;
    }
    cJSON *reply = cJSON_Parse(net_worker_result.data);
    ok = reply && (pairing ? (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply , "paired")) &&
                              !strcmp(jsontext(reply , "device_id") , net_id)) :
                             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply , "ok")));
    cJSON_Delete(reply);
    if (!ok)
    {
        return false;
    }
    net_config promoted = net_candidate;
    promoted.code[0] = 0;
    if (!netconfigsave("active" , promoted))
    {
        netstatus("CONFIG_SAVE_FAILED");
        return false;
    }
    net_active = promoted;
    net_prefs.remove("candidate");
    net_candidate_pending = false;
    net_worker_using_candidate = false;
    if (net_worker_ap)
    {
        netcloseportal();
    }
    return true;
}

void networktask(void *)
{
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    net_prefs.begin("attendance-net" , false);
    netconfigread("active" , net_active);
    net_candidate_pending = netconfigread("candidate" , net_candidate);
    netportalroutes();
    configTime(0 , 0 , "time.cloudflare.com" , "ntp.aliyun.com" , "pool.ntp.org");
    if (net_candidate_pending || net_active.ssid[0])
    {
        netconnect(net_candidate_pending);
    }
    else
    {
        netopenportal();
        netstatus("SETUP_REQUIRED");
    }
    const char *paths[] = {"" , "events" , "roster" , "heartbeat" , "commands" , "command-ack" , "time"};
    for (;;)
    {
        uint8_t control;
        if (xQueueReceive(net_controls , &control , 0) == pdTRUE)
        {
            netopenportal();
        }
        if (net_worker_ap)
        {
            net_dns.processNextRequest();
            net_server.handleClient();
            if (millis() - net_ap_at >= 600000)
            {
                netcloseportal();
            }
        }
        uint32_t now = millis();
        bool connected = WiFi.status() == WL_CONNECTED;
        bool ready = connected && !net_worker_using_candidate && net_active.ssid[0];
        if (connected && net_worker_using_candidate && netdue(now , net_worker_try_at))
        {
            net_worker_try_at = now + 10000;
            ready = netpaircandidate();
        }
        if (net_worker_using_candidate && now - net_worker_connect_at >= 120000 && net_active.ssid[0])
        {
            netconnect(false);
            netstatus("NEW_CONFIG_FAILED");
        }
        if (!connected && net_active.ssid[0] && !net_worker_using_candidate && now - net_worker_connect_at >= 30000)
        {
            netconnect(false);
        }
        int32_t rssi = connected ? WiFi.RSSI() : -127;
        portENTER_CRITICAL(&net_lock);
        net_ready = ready;
        net_rssi = rssi;
        portEXIT_CRITICAL(&net_lock);
        if (!connected)
        {
            netstatus(net_worker_ap ? "SETUP_HOTSPOT_OFFLINE" : "HOTSPOT_OFFLINE");
        }
        if (xQueueReceive(net_jobs , &net_worker_job , pdMS_TO_TICKS(20)) == pdTRUE)
        {
            const char *payload = net_worker_job.kind == net_commands || net_worker_job.kind == net_time ? nullptr : net_worker_job.payload;
            net_worker_result.status = 0;
            net_worker_result.ok = ready && netrequest(paths[net_worker_job.kind] , payload , net_active , net_worker_result.data , &net_worker_result.status);
            if (net_worker_result.ok)
            {
                netstatus("ONLINE");
            }
            xQueueSend(net_results , &net_worker_result , portMAX_DELAY);
        }
    }
}

bool netserialize(cJSON *data)
{
    bool ok = data && cJSON_PrintPreallocated(data , net_outgoing.payload , sizeof(net_outgoing.payload) , false);
    cJSON_Delete(data);
    if (!ok)
    {
        netstatus("JSON_TOO_LARGE");
    }
    return ok;
}

#include "network_sync.h"

void initnetwork()
{
    setenv("TZ" , "UTC0" , 1);
    tzset();
    uint64_t mac = ESP.getEfuseMac();
    snprintf(net_id , sizeof(net_id) , "s3-%04x%08lx" , static_cast<unsigned>((mac >> 32) & 0xFFFF) ,
             static_cast<unsigned long>(mac & 0xFFFFFFFF));
    if (clockvalid())
    {
        struct tm value = {};
        if (strptime(clocktext() , "%Y-%m-%d %H:%M:%S" , &value))
        {
            time_t epoch = mktime(&value) - 8 * 3600;
            struct timeval tv = {epoch , 0};
            settimeofday(&tv , nullptr);
        }
    }
    net_jobs = xQueueCreate(1 , sizeof(net_job));
    net_results = xQueueCreate(1 , sizeof(net_reply));
    net_controls = xQueueCreate(1 , sizeof(uint8_t));
    if (!net_jobs || !net_results || !net_controls ||
        xTaskCreatePinnedToCore(networktask , "attendance_net" , 16384 , nullptr , 1 , &net_task , 0) != pdPASS)
    {
        netstatus("NET_TASK_FAILED");
    }
}

void pollnetwork()
{
    if (!net_task)
    {
        return;
    }
    uint32_t now = millis();
    if (buttonpressed())
    {
        if (!net_button_held)
        {
            net_button_at = now;
            net_button_held = true;
        }
        if (!net_button_fired && now - net_button_at >= 3000)
        {
            uint8_t control = 1;
            xQueueSend(net_controls , &control , 0);
            net_button_fired = true;
        }
    }
    else
    {
        net_button_held = net_button_fired = false;
    }
    if (attendancechanged() != net_seen_change)
    {
        net_seen_change = attendancechanged();
        if (!strcmp(attendancestate() , "SAVED"))
        {
            net_pass_at = now + 1000;
        }
    }
    if (net_inflight)
    {
        if (xQueueReceive(net_results , &net_result , 0) == pdTRUE)
        {
            net_inflight = false;
            if (!net_result.ok || !nethandlereply())
            {
                net_failures ++;
                if (netremotescan() && net_outgoing.kind == net_events &&
                    (net_result.status == 400 || net_result.status == 409 || net_result.status == 413))
                {
                    netscanerror("UPLOAD_REJECTED");
                    return;
                }
                net_retry = true;
                net_retry_at = now + 15000;
            }
        }
        return;
    }
    portENTER_CRITICAL(&net_lock);
    bool ready = net_ready;
    portEXIT_CRITICAL(&net_lock);
    if (!ready)
    {
        return;
    }
    if (net_retry)
    {
        if (netdue(now , net_retry_at))
        {
            netsend();
        }
        return;
    }
    if (net_command_id[0] && !net_command_acking)
    {
        if (!strcmp(net_command_kind , "clock"))
        {
            net_outgoing.kind = net_time;
            netsend();
            return;
        }
        if (!strcmp(net_command_kind , "roster"))
        {
            if (!netbuildroster())
            {
                netfinishtask(false , "SD_OR_REGISTRY_ERROR");
            }
            return;
        }
    }
    if (netdue(now , net_time_at))
    {
        net_outgoing.kind = net_time;
        netsend();
        return;
    }
    if (netdue(now , net_heartbeat_at))
    {
        cJSON *object = cJSON_CreateObject();
        cJSON_AddStringToObject(object , "version" , "0.10.0");
        cJSON *capabilities = cJSON_AddArrayToObject(object , "capabilities");
        cJSON_AddItemToArray(capabilities , cJSON_CreateString("clock"));
        cJSON_AddItemToArray(capabilities , cJSON_CreateString("roster"));
        cJSON_AddItemToArray(capabilities , cJSON_CreateString("rescan"));
        cJSON_AddStringToObject(object , "oled" , displayready() ? "INITIALIZED" : "OFFLINE");
        cJSON_AddStringToObject(object , "registry" , attendancehealthy() ? "READY" : "OFFLINE");
        cJSON_AddStringToObject(object , "rtc" , clockvalid() ? clocktext() : "INVALID");
        cJSON_AddStringToObject(object , "sd" , storageready() ? "MOUNTED" : "OFFLINE");
        cJSON_AddStringToObject(object , "pn532" , nfcready() ? "READY" : "OFFLINE");
        cJSON_AddStringToObject(object , "last_uid" , carduid());
        cJSON_AddStringToObject(object , "state" , attendancestate());
        cJSON_AddNumberToObject(object , "members" , attendancemembers());
        cJSON_AddNumberToObject(object , "today" , attendancetoday());
        cJSON_AddNumberToObject(object , "rssi" , net_rssi);
        net_outgoing.kind = net_heartbeat;
        if (netserialize(object))
        {
            netsend();
        }
        return;
    }
    if (!net_command_id[0] && netdue(now , net_commands_at))
    {
        net_outgoing.kind = net_commands;
        netsend();
        return;
    }
    if (net_seen_members != attendancemembers() || netdue(now , net_roster_at))
    {
        if (netbuildroster())
        {
            return;
        }
    }
    netnextrecord();
}

void networkview(char *state , char *ssid , char *password , bool &portal)
{
    portENTER_CRITICAL(&net_lock);
    strcpy(state , net_state);
    strcpy(ssid , net_ap_ssid);
    strcpy(password , net_ap_password);
    portal = net_portal;
    portEXIT_CRITICAL(&net_lock);
}

void reportnetwork(Stream &out)
{
    char state[32] , ssid[24] , password[17];
    bool portal;
    networkview(state , ssid , password , portal);
    out.printf("network_id=%s network_state=%s portal=%u uploaded_acks=%lu failures=%lu\n" ,
               net_id , state , portal , static_cast<unsigned long>(net_uploaded) , static_cast<unsigned long>(net_failures));
}
