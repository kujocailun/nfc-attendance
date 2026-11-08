#pragma once

//主循环使用的同步与任务逻辑；传输层在 network.cpp。
bool netsend()
{
    if (xQueueSend(net_jobs , &net_outgoing , 0) != pdTRUE)
    {
        return false;
    }
    net_inflight = true;
    net_retry = false;
    return true;
}

bool netfinishtask(bool ok , const char *detail)
{
    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack , "id" , net_command_id);
    cJSON_AddBoolToObject(ack , "ok" , ok);
    cJSON_AddStringToObject(ack , "detail" , detail);
    net_outgoing.kind = net_ack;
    if (!netserialize(ack))
    {
        return false;
    }
    net_command_acking = true;
    net_retry = true;
    net_retry_at = millis();
    return true;
}

bool netremotescan()
{
    return !net_command_acking && !strcmp(net_command_kind , "rescan");
}

void netscanerror(const char *detail)
{
    net_file.close();
    netstatus(detail);
    if (netremotescan())
    {
        net_directory.close();
        net_pass_at = millis() + 300000;
        netfinishtask(false , detail);
    }
}

void nethash()
{
    char input[64];
    snprintf(input , sizeof(input) , "%s\n%.10s\n%s" , net_id , net_stamp , net_uid);
    uint8_t result[32];
    mbedtls_sha256_ret(reinterpret_cast<const uint8_t *>(input) , strlen(input) , result , 0);
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0 ; i < sizeof(result) ; i ++)
    {
        net_expected[i * 2] = hex[result[i] >> 4];
        net_expected[i * 2 + 1] = hex[result[i] & 15];
    }
    net_expected[64] = 0;
}

//本函数只由 Arduino 主循环调用，网络任务不接触 SD。
bool netnextrecord()
{
    if (!storageready() || !attendancehealthy())
    {
        if (netremotescan())
        {
            netscanerror("SD_OR_REGISTRY_ERROR");
        }
        return false;
    }
    if (!net_directory)
    {
        if (!netdue(millis() , net_pass_at))
        {
            return false;
        }
        net_directory = SD.open("/attendance" , FILE_READ);
        if (!net_directory)
        {
            net_pass_at = millis() + 30000;
            if (netremotescan())
            {
                File root = SD.open("/" , FILE_READ);
                bool empty = root && root.isDirectory() && !SD.exists("/attendance");
                root.close();
                if (empty)
                {
                    netfinishtask(true , "HISTORY_SCAN_ACKED");
                }
                else
                {
                    netscanerror("SD_OPEN_FAILED");
                }
            }
            return false;
        }
    }
    if (!net_file)
    {
        net_file = net_directory.openNextFile();
        if (!net_file)
        {
            net_directory.close();
            net_pass_at = millis() + 300000;
            if (netremotescan())
            {
                netfinishtask(true , "HISTORY_SCAN_ACKED");
            }
            return false;
        }
        const char *name = strrchr(net_file.name() , '/');
        name = name ? name + 1 : net_file.name();
        if (net_file.isDirectory() || strlen(name) != 14 || strcmp(name + 10 , ".csv"))
        {
            net_file.close();
            return false;
        }
        memcpy(net_day , name , 10);
        net_day[10] = 0;
        if (!attendanceday(net_day) || net_file.readBytesUntil('\n' , net_line , sizeof(net_line)) != 20)
        {
            netscanerror("SD_HISTORY_INVALID");
            return false;
        }
    }
    if (!net_file.available())
    {
        net_file.close();
        return false;
    }
    size_t length = 0;
    bool complete = false;
    while (net_file.available() && length + 1 < sizeof(net_line))
    {
        signed c = net_file.read();
        if (c == '\n')
        {
            complete = true;
            break;
        }
        net_line[length ++] = static_cast<char>(c);
    }
    net_line[length] = 0;
    if (!complete || !attendancerecord(net_line , net_stamp , net_uid , net_name) || strncmp(net_stamp , net_day , 10))
    {
        netscanerror("SD_HISTORY_INVALID");
        return false;
    }
    nethash();
    cJSON *object = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(object , "events");
    cJSON *row = cJSON_CreateObject();
    cJSON_AddItemToArray(array , row);
    cJSON_AddStringToObject(row , "event_id" , net_expected);
    cJSON_AddStringToObject(row , "time" , net_stamp);
    cJSON_AddStringToObject(row , "uid" , net_uid);
    cJSON_AddStringToObject(row , "name" , net_name);
    cJSON_AddStringToObject(row , "status" , "SAVED");
    net_outgoing.kind = net_events;
    if (!netserialize(object))
    {
        net_file.close();
        net_directory.close();
        return false;
    }
    return netsend();
}

bool netbuildroster()
{
    if (!attendancehealthy())
    {
        return false;
    }
    cJSON *object = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(object , "members");
    for (uint16_t i = 0 ; i < attendancemembers() ; i ++)
    {
        if (!attendancemember(i , net_uid , net_name))
        {
            cJSON_Delete(object);
            return false;
        }
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row , "uid" , net_uid);
        cJSON_AddStringToObject(row , "name" , net_name);
        cJSON_AddItemToArray(array , row);
    }
    net_roster_sent_count = attendancemembers();
    net_outgoing.kind = net_roster;
    return netserialize(object) && netsend();
}

bool netcommand(cJSON *reply)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(reply , "commands");
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > 1)
    {
        return false;
    }
    if (!cJSON_GetArraySize(array))
    {
        return true;
    }
    cJSON *command = cJSON_GetArrayItem(array , 0);
    const char *key = jsontext(command , "id");
    const char *uid = jsontext(command , "uid");
    const char *name = jsontext(command , "name");
    const char *kind = jsontext(command , "kind");
    if (strlen(key) != 32 || net_command_id[0])
    {
        return false;
    }
    if (strcmp(kind , "bind") && strcmp(kind , "clock") && strcmp(kind , "roster") && strcmp(kind , "rescan"))
    {
        return false;
    }
    for (size_t i = 0 ; i < 32 ; i ++)
    {
        if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f')))
        {
            return false;
        }
    }
    strcpy(net_command_id , key);
    strcpy(net_command_kind , kind);
    if (!strcmp(kind , "bind"))
    {
        bool valid = (strlen(uid) == 11 || strlen(uid) == 20) && strlen(name) && strlen(name) <= 48;
        bool ok = valid && attendancebind(uid , name , Serial);
        net_roster_at = millis();
        return netfinishtask(ok , ok ? "SD_READBACK_OK" : "SD_OR_REGISTRY_ERROR");
    }
    if (!strcmp(kind , "rescan"))
    {
        net_file.close();
        net_directory.close();
        net_pass_at = millis();
    }
    return true;
}

bool nethandlereply()
{
    cJSON *reply = cJSON_Parse(net_result.data);
    if (!reply)
    {
        return false;
    }
    bool ok = false;
    uint32_t now = millis();
    if (net_outgoing.kind == net_events)
    {
        cJSON *acks = cJSON_GetObjectItemCaseSensitive(reply , "acked");
        cJSON *item = cJSON_GetArrayItem(acks , 0);
        ok = cJSON_IsArray(acks) && cJSON_GetArraySize(acks) == 1 && cJSON_IsString(item) && !strcmp(item->valuestring , net_expected);
        net_uploaded += ok;
    }
    else if (net_outgoing.kind == net_roster)
    {
        cJSON *count = cJSON_GetObjectItemCaseSensitive(reply , "count");
        ok = cJSON_IsNumber(count) && count->valueint == net_roster_sent_count;
        if (ok)
        {
            net_seen_members = net_roster_sent_count;
            net_roster_at = now + 300000;
            if (!strcmp(net_command_kind , "roster"))
            {
                ok = netfinishtask(true , "ROSTER_UPLOADED");
            }
        }
    }
    else if (net_outgoing.kind == net_heartbeat)
    {
        ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply , "ok"));
        net_heartbeat_at = now + 60000;
    }
    else if (net_outgoing.kind == net_commands)
    {
        ok = netcommand(reply);
        net_commands_at = now + 30000;
    }
    else if (net_outgoing.kind == net_ack)
    {
        cJSON *sent = cJSON_Parse(net_outgoing.payload);
        ok = sent && !strcmp(jsontext(reply , "acked") , jsontext(sent , "id"));
        cJSON_Delete(sent);
        if (ok)
        {
            net_command_id[0] = net_command_kind[0] = 0;
            net_command_acking = false;
            net_heartbeat_at = net_commands_at = now;
        }
    }
    else if (net_outgoing.kind == net_time)
    {
        ok = clocksettext(jsontext(reply , "time") , Serial);
        net_time_at = now + 21600000;
        if (!strcmp(net_command_kind , "clock"))
        {
            ok = netfinishtask(ok , ok ? "RTC_READBACK_OK" : "RTC_VERIFY_FAILED");
        }
    }
    cJSON_Delete(reply);
    return ok;
}
