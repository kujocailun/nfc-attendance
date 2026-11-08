"use strict";

let networkData = {devices: [] , commands: []};
const remoteNames = {bind: "登记新卡" , clock: "立即校时" , roster: "同步名单" , rescan: "重扫历史记录"};
const remoteResults = {SD_READBACK_OK: "卡片已写入并读回确认" , RTC_READBACK_OK: "时钟已校准并读回确认" , ROSTER_UPLOADED: "终端名单已上传" , HISTORY_SCAN_ACKED: "本轮历史扫描结束，读取到的记录均已获后台确认" , SD_OR_REGISTRY_ERROR: "SD 或名单异常" , RTC_VERIFY_FAILED: "时钟校准或读回失败" , SD_HISTORY_INVALID: "历史文件损坏，需检查 SD" , SD_OPEN_FAILED: "无法打开历史目录" , UPLOAD_REJECTED: "后台拒绝历史记录，请检查数据冲突"};

function networkTerminal()
{
    return networkData.devices.find((d) => !d.revoked);
}

function initNetwork()
{
    const page = $("#page-device");
    const maintenance = document.createElement("details");
    maintenance.id = "usb-maintenance";
    maintenance.className = "usb-maintenance";
    maintenance.innerHTML = '<summary>USB 维护<span>本地接线、指定日期读取与串口诊断</span></summary>';
    Array.from(page.children).slice(1).forEach((node) => maintenance.append(node));
    const section = document.createElement("div");
    section.id = "network-management";
    section.innerHTML = `
        <article class="panel"><div class="panel-head"><div><h2>联网终端</h2><p>无需 USB，连接已保存的手机热点即可管理。</p></div><button id="net-refresh" class="button small">刷新状态</button></div><div id="net-devices" class="panel-body"></div></article>
        <article class="panel"><div class="panel-head"><h2>远程操作</h2><span class="badge neutral">执行后由终端确认</span></div><div class="panel-body"><div class="actions remote-actions"><button class="button remote-operation" data-kind="clock" disabled>立即校时</button><button class="button remote-operation" data-kind="roster" disabled>同步名单</button><button class="button remote-operation" data-kind="rescan" disabled>重扫历史记录</button><a href="/api/backup" class="button">下载数据库备份</a></div><p id="net-operation-hint" class="hint">等待终端状态。</p><p class="hint">离线时任务保留在后台，恢复联网后执行。校时采用执行时后台电脑的北京时间；重扫读取全部历史，重复记录会去重。</p></div></article>
        <div class="device-grid">
            <article class="panel"><div class="panel-head"><h2>登记新卡</h2></div><form id="net-bind-form" class="panel-body"><label class="form-field">目标终端<select id="net-device"></select></label><label class="form-field">卡片 UID<div class="input-action"><input id="net-uid" maxlength="20" placeholder="04-11-22-33" required><button id="net-use-last" class="button small" type="button">使用最近刷卡</button></div></label><label class="form-field">绑定姓名<input id="net-name" maxlength="48" placeholder="如 student" required></label><button id="net-bind" class="button primary" type="submit">提交登记任务</button><p class="hint">先刷一次新卡，等待下次状态更新后获取 UID。终端写入 SD 并读回名单后才显示完成。</p></form></article>
            <article class="panel"><div class="panel-head"><h2>配对与热点设置</h2></div><div class="panel-body"><label class="form-field">终端名称<input id="net-label" maxlength="40" value="实验室签到终端"></label><button id="net-pair" class="button">生成一次性配对码</button><p id="net-code" class="mono net-code" hidden></p><p class="hint">首次配对或撤销后重配时生成，有效期 10 分钟。长按终端按键 3 秒，手机连接 OLED 上的 SETUP Wi-Fi，打开 192.168.4.1 填写热点和配对码。</p><p class="hint">保存后断开 SETUP Wi-Fi，再开启安卓热点，使用 2.4GHz / WPA2 兼容模式。仅更换热点时，已配对终端可留空配对码。</p></div></article>
        </div>
        <article class="panel"><div class="panel-head"><h2>远程任务记录</h2><span class="hint">待领取 → 已下发 → 完成 / 失败</span></div><div id="net-commands" class="table-wrap"></div></article>`;
    page.append(section , maintenance);
    $("#page-device .subtitle").textContent = "通过手机热点远程管理，USB 用于现场维护。";
    $("#page-device .page-heading > .badge").textContent = "后台 v2.1.0";
    document.querySelector("footer span:last-child").textContent = "v2.1.0 · 数据保存在部署电脑";
    document.querySelector(".local-label").textContent = "Wi-Fi / USB 管理";
    document.querySelector(".profile small").textContent = "后台管理员";
    document.querySelector(".local-dot").title = "管理员访问受保护";
    $("#quick-sync").innerHTML = icon("sync") + "同步记录";
    $("#record-sync").innerHTML = icon("sync") + "同步记录";
    $("#page-records .info-note").textContent = "查询日期用于筛选后台记录。联网时重扫终端全部历史；USB 维护可按指定日期读取。重复上传不会增加签到记录。";
    $("#page-guide .guide").innerHTML = `<h2>日常签到</h2><p>开启安卓手机的固定热点，再给终端通电。终端离线仍可刷卡，记录保存在 SD，恢复联网后自动补传。</p><h2>终端管理</h2><p>在“终端管理”查看最近通信时间、模块状态和最近卡号。状态约每分钟上报，页面刷新不会强制终端立即上报。登记新卡及远程操作通过任务队列执行，请查看终端回执。</p><p>立即校时、同步名单和重扫历史需要固件 v0.10.0。旧版 v0.9.0 仍支持状态上报、自动补传和远程登记。每次重扫只确认本轮读取的记录，不将某天标记为完整同步。</p><h2>USB 维护</h2><p>展开“USB 维护”，将 COM 口连接到运行后台的电脑后选择串口。可读取指定日期、手动校时和查看原始串口日志。远程浏览器显示的是部署电脑的串口。</p><h2>数据与统计</h2><p>签到记录可按日期查询并导出 CSV。人员编号、分组和备注保存在后台；绑定姓名和 UID 保存在终端。当前不支持删除卡片或修改绑定姓名，一人多卡会分别计数。</p><p>“当日未见签到”只对比已收到的记录和当前名单，不能直接认定缺勤。数据库可随时下载一致性备份，恢复前需停止后台。第一版支持一台终端、一个管理员。</p>`;
    $("#net-refresh").onclick = (e) => action(e.currentTarget , refreshNetwork);
    $("#net-pair").onclick = (e) => action(e.currentTarget , async () =>
    {
        const data = await api("network/pair" , {label: $("#net-label").value});
        $("#net-code").hidden = false;
        $("#net-code").textContent = data.code + " · 10 分钟内使用";
        notify("配对码已生成，请在终端配网页填写。");
    });
    document.querySelectorAll(".remote-operation").forEach((button) => button.onclick = () =>
    {
        action(button , () => queueRemote(button.dataset.kind)).finally(updateRemoteButtons);
    });
    $("#net-use-last").onclick = () =>
    {
        const terminal = networkTerminal();
        const uid = terminal && terminal.telemetry.last_uid;
        if (!uid)
        {
            notify("请先刷卡，等待终端下一次状态更新。" , true);
            return;
        }
        $("#net-uid").value = uid;
        notify("已填入最近上报的卡号，请核对是否为本次要登记的卡片。");
    };
    $("#net-bind-form").onsubmit = (e) =>
    {
        e.preventDefault();
        action($("#net-bind") , async () =>
        {
            const data = await api("network/bind" , {device_id: $("#net-device").value , uid: $("#net-uid").value , name: $("#net-name").value});
            notify(data.message);
            await refreshNetwork();
        });
    };
}

async function queueRemote(kind)
{
    const terminal = networkTerminal();
    if (!terminal)
    {
        throw new Error("请先配对终端。");
    }
    const result = await api("network/command" , {device_id: terminal.id , kind});
    notify(result.message);
    await refreshNetwork();
}

function updateRemoteButtons()
{
    const terminal = networkTerminal();
    const capabilities = terminal ? terminal.capabilities || [] : [];
    document.querySelectorAll(".remote-operation").forEach((button) =>
    {
        const pending = terminal && networkData.commands.some((c) => c.device_id === terminal.id && c.kind === button.dataset.kind && ["pending" , "running"].includes(c.state));
        button.disabled = !capabilities.includes(button.dataset.kind) || Boolean(pending);
        button.title = pending ? "已有相同任务，等待终端回执" : capabilities.includes(button.dataset.kind) ? "" : "需要固件 v0.10.0 上报此能力";
    });
    const hint = $("#net-operation-hint");
    if (hint)
    {
        hint.textContent = !terminal ? "配对终端后可以远程管理。" : !capabilities.length ? "当前固件未上报远程维护能力。升级至 v0.10.0 后启用以上操作；远程登记仍可使用。" : terminal.online ? "终端在线，通常约 30 秒内领取任务；历史重扫耗时取决于记录数量。" : "终端离线，可以提交任务，恢复网络后执行。";
    }
}

async function refreshNetwork()
{
    if (!$("#net-devices"))
    {
        return;
    }
    networkData = await api("network");
    const selected = $("#net-device").value;
    const devices = networkData.devices;
    const activeDevice = networkTerminal();
    window.__netActive = Boolean(activeDevice);
    window.__netOnline = Boolean(activeDevice && activeDevice.online);
    renderState();
    $("#net-devices").innerHTML = devices.length ? devices.map((d) =>
    {
        const t = d.telemetry;
        const seen = d.seen ? new Date(d.seen * 1000).toLocaleString("zh-CN" , {timeZone: "Asia/Shanghai"}) : "尚未收到心跳";
        return `<div class="net-device-row"><div><strong>${esc(d.label)}</strong> <span class="badge ${d.online ? "" : "neutral"}">${d.revoked ? "已撤销" : d.online ? "在线" : "离线"}</span><p class="mono">${esc(d.id)} · 固件 ${esc(t.version || "待上报")}</p><p>最近通信 ${esc(seen)} · 信号 ${esc(t.rssi ?? "—")} dBm</p><p>读卡器 ${esc(t.pn532 || "待上报")} · SD ${esc(t.sd || "待上报")} · OLED ${esc(t.oled || "待上报")} · 名单 ${esc(t.registry || "待上报")}</p><p>终端时间 ${esc(t.rtc || "待上报")}（最近上报值）</p><p>今日 ${esc(t.today ?? "—")} · 登记卡片 ${esc(t.members ?? "—")} · 最近卡号 <span class="mono">${esc(t.last_uid || "暂无")}</span> · ${esc(states[t.state] || t.state || "等待刷卡")}</p></div>${d.revoked ? "" : `<button class="button small net-revoke" data-id="${esc(d.id)}">撤销配对</button>`}</div>`;
    }).join("") : empty("尚未配对联网终端" , "生成配对码，在终端配网页填写后连接手机热点。");
    const active = devices.filter((d) => !d.revoked);
    $("#net-device").innerHTML = active.length ? active.map((d) => `<option value="${esc(d.id)}">${esc(d.label)} · ${esc(d.id)}</option>`).join("") : '<option value="">请先配对终端</option>';
    if (active.some((d) => d.id === selected))
    {
        $("#net-device").value = selected;
    }
    $("#net-bind").disabled = !active.length;
    const labels = {pending: "待终端领取" , running: "已下发，等待确认" , done: "终端已确认" , failed: "执行失败"};
    $("#net-commands").innerHTML = networkData.commands.length ? `<table><thead><tr><th>操作</th><th>提交时间</th><th>状态</th><th>终端回执</th></tr></thead><tbody>${networkData.commands.map((c) =>
    {
        const p = JSON.parse(c.payload);
        const subject = c.kind === "bind" ? `${esc(p.name)}<br><span class="mono">${esc(p.uid)}</span>` : esc(remoteNames[c.kind] || c.kind);
        return `<tr><td>${subject}</td><td>${esc(c.created_at)}</td><td><span class="badge ${c.state === "done" ? "" : c.state === "failed" ? "warning" : "neutral"}">${labels[c.state] || esc(c.state)}</span></td><td>${esc(remoteResults[c.result] || c.result || (c.state === "running" ? "终端断网或重启后会重试，尚未收到完成回执" : "等待联网领取"))}</td></tr>`;
    }).join("")}</tbody></table>` : empty("暂无远程任务" , "登记或提交操作后，可以在这里查看终端回执。");
    updateRemoteButtons();
    document.querySelectorAll(".net-revoke").forEach((button) => button.onclick = () =>
    {
        if (window.confirm("撤销后终端停止联网同步，需要重新配对。继续吗？"))
        {
            action(button , async () =>
            {
                await api("network/revoke" , {device_id: button.dataset.id});
                await refreshNetwork();
            });
        }
    });
}
