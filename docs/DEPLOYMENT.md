# Windows 上线部署

## 准备

需要一台长期运行的 Windows 电脑、Python 3.11 x64、自己的 Cloudflare 托管域名和 `cloudflared`。下文统一使用保留域名 `example.com`；邮箱、Access 团队名和 AUD 均为占位内容，必须替换。

从仓库下载源码，在 PowerShell 运行安装脚本。它会创建 `admin/.venv`，有 `wheelhouse` 时离线安装，否则从 Python 包索引安装锁定依赖。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ".\admin\deploy\install.ps1"
```

## Cloudflare 配置

在 Zero Trust 中创建 Tunnel，把两个 Public Hostname 都指向 `http://127.0.0.1:18080`：

| 域名示例 | 用途 | Access 策略 |
| --- | --- | --- |
| `checkin.example.com` | 浏览器管理 | Self-hosted 应用，仅允许自己的管理员邮箱 |
| `device-api.example.com` | ESP32 HTTPS 请求 | 不套交互登录页，由后台验证设备密钥 |

使用控制台提供的官方 Windows connector 安装步骤，在生产电脑把 `cloudflared` 安装为服务。Tunnel Token 只在生产电脑输入，不能截图提交到仓库、写入脚本或发给协作者。不要给两个域名配置成“对所有人放行”的管理入口。

记录管理 Access 应用的 AUD 和团队 issuer，issuer 形如 `https://your-team.cloudflareaccess.com`。Access allow 策略配置自己的管理员邮箱，应用域名必须与后台管理域名一致。设备域名无需邮箱登录，其他用户请求仍会被设备认证拒绝。

## 生成本地生产配置

替换以下参数后执行。PowerShell 续行反引号后不能有空格；也可以把命令合并成一行。

```powershell
admin/.venv/Scripts/python.exe admin/deploy/prepare.py `
  --admin-host checkin.example.com `
  --device-host device-api.example.com `
  --admin-email admin@example.com `
  --issuer https://your-team.cloudflareaccess.com `
  --audience YOUR_ACCESS_APPLICATION_AUD
```

脚本生成被 Git 忽略的 `admin/network-config.json`，将 `local_admin` 设为 `false`，已有配置会备份到私有 `deploy-state/`。实际域名和邮箱也视为本地部署信息，不提交 Git。

先前台运行一次：

```powershell
admin/.venv/Scripts/python.exe admin/network_server.py
```

浏览器打开自己的管理域名，通过 Access 邮箱登录。检查空名单、总览和设备管理能打开。生产模式直接访问 `127.0.0.1:18080` 被拒绝是预期行为，应使用管理域名；不要为方便访问而改回 `local_admin=true`。

确认前台进程正常后停止它，再用管理员 PowerShell 注册后台启动任务：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ".\admin\deploy\register-startup.ps1"
Get-ScheduledTask -TaskName NFC-Attendance-Backend
```

任务使用当前 Windows 账号的 S4U 启动，无需交互登录。云隧道由自己的 Windows 服务运行。两者都是启动链路的一部分，应分别检查；电脑关闭、休眠、断网时公网后台也会中断。设置合适的电源策略，并在生产电脑实测重启后的服务恢复。

移除后台启动任务可运行 `admin/deploy/unregister-startup.ps1`，该操作不删除数据库。部署失败时检查 `admin/data/network.log`，分享日志前先脱敏。

## 固件和首次配对

将固件的 `src/project_config.h` 设置为自己的设备 HTTPS URL，编译并通过 COM 口烧录，详见主 README。域名需已通过 Tunnel 提供服务，设备联网时能解析域名并校验证书。

打开管理页面，生成设备配对码。配对码为一次性凭据，有效 10 分钟。长按终端按键约 3 秒，按 OLED 提示连接终端的 `SETUP-…` AP，访问 `192.168.4.1` 输入热点信息和配对码。保存后断开该 AP，开启安卓 2.4 GHz/WPA2 热点。

终端生成随机设备密钥并存入 NVS，后台只存摘要。设备 ID 基于芯片标识生成，不应作为截图中的匿名标识。配对完成后代码不保存有效配对码。已配对设备更换热点可留空配对码，正式配置仅在服务器校验成功后替换。

TLS 需要可信系统时间，终端启动会请求网络时间。网络限制 NTP、域名解析或 HTTPS 时应分别排查，不能关闭证书验证来绕过。

## 本地网络版开发

未接入 Tunnel 时，可复制 `admin/network-config.example.json` 到 `admin/network-config.json`，保持 `local_admin=true`，运行网络后台并打开 `http://127.0.0.1:18080`。这个模式用于开发电脑，不给互联网开放端口。

终端请求固定 HTTPS 设备域名，不能仅靠上述本地入口直接连入。实物 Wi-Fi 联调仍需要可达的 HTTPS 设备入口；USB 本地管理则可以完全离线。

## 更新与回退

更新前停后台并备份 `admin/data/`、`admin/network-config.json`、终端 SD 和本地 `project_config.h`。拉取新源码后按锁定依赖安装，再启动后台，检查实际版本及数据库数据。当前版本在旧库上增量添加网络命令表字段。

Git 回退不会回退数据库。涉及数据库结构变化时，需按对应版本说明处理并保留升级前备份。不要把本地旧交付包、数据库迁移文件夹或部署 Token 一起推送。
