#pragma once

//测试替身仅保存 JSON 对象树，协议解析与 TLS 由 ESP32 编译及后台集成测试覆盖。
struct cJSON
{
    signed type = 0 , valueint = 0;
    std::string text , name;
    const char *valuestring = nullptr;
    std::vector<cJSON *> children;
};

inline std::map<std::string , cJSON *> host_json;
inline size_t host_json_number;

inline cJSON *cJSON_CreateObject()
{
    return new cJSON();
}

inline cJSON *cJSON_CreateString(const char *value)
{
    auto *node = new cJSON();
    node->type = 2;
    node->text = value;
    node->valuestring = node->text.c_str();
    return node;
}

inline void cJSON_Delete(cJSON *node)
{
    if (!node)
    {
        return;
    }
    for (auto *child : node->children)
    {
        cJSON_Delete(child);
    }
    delete node;
}

inline cJSON *host_clone(const cJSON *node)
{
    auto *copy = new cJSON(*node);
    copy->valuestring = copy->text.c_str();
    copy->children.clear();
    for (const auto *child : node->children)
    {
        copy->children.push_back(host_clone(child));
    }
    return copy;
}

inline cJSON *cJSON_AddArrayToObject(cJSON *node , const char *name)
{
    auto *array = new cJSON();
    array->type = 1;
    array->name = name;
    node->children.push_back(array);
    return array;
}

inline void cJSON_AddItemToArray(cJSON *array , cJSON *item)
{
    array->children.push_back(item);
}

inline void cJSON_AddStringToObject(cJSON *node , const char *name , const char *value)
{
    auto *item = cJSON_CreateString(value);
    item->name = name;
    node->children.push_back(item);
}

inline void cJSON_AddBoolToObject(cJSON *node , const char *name , bool value)
{
    auto *item = new cJSON();
    item->type = 3;
    item->valueint = value;
    item->name = name;
    node->children.push_back(item);
}

inline void cJSON_AddNumberToObject(cJSON *node , const char *name , signed value)
{
    auto *item = new cJSON();
    item->type = 4;
    item->valueint = value;
    item->name = name;
    node->children.push_back(item);
}

inline cJSON *cJSON_GetObjectItemCaseSensitive(cJSON *node , const char *name)
{
    if (node)
    {
        for (auto *child : node->children)
        {
            if (child->name == name)
            {
                return child;
            }
        }
    }
    return nullptr;
}

inline bool cJSON_IsString(cJSON *node)
{
    return node && node->type == 2;
}
inline bool cJSON_IsArray(cJSON *node)
{
    return node && node->type == 1;
}
inline bool cJSON_IsNumber(cJSON *node)
{
    return node && node->type == 4;
}
inline bool cJSON_IsTrue(cJSON *node)
{
    return node && node->type == 3 && node->valueint;
}
inline size_t cJSON_GetArraySize(cJSON *node)
{
    return node ? node->children.size() : 0;
}
inline cJSON *cJSON_GetArrayItem(cJSON *node , size_t index)
{
    return node && index < node->children.size() ? node->children[index] : nullptr;
}

inline bool cJSON_PrintPreallocated(cJSON *node , char *buffer , size_t capacity , bool)
{
    std::string key = "host-json-" + std::to_string(++ host_json_number);
    if (key.size() + 1 > capacity)
    {
        return false;
    }
    host_json[key] = host_clone(node);
    std::strcpy(buffer , key.c_str());
    return true;
}

inline cJSON *cJSON_Parse(const char *buffer)
{
    auto it = host_json.find(buffer);
    return it == host_json.end() ? nullptr : host_clone(it->second);
}
