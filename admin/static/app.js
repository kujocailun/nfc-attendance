"use strict";

const $ = (selector) => document.querySelector(selector);
const pages = {overview: "签到总览" , records: "签到记录" , members: "人员与卡片" , device: "终端管理" , guide: "使用指南"};
const states = {SAVED: "签到成功" , ALREADY_TODAY: "今日已签到" , UNKNOWN_CARD: "未登记卡片" , CLOCK_ERROR: "时钟异常" , SD_OFFLINE: "SD 未就绪" , REGISTRY_ERROR: "名单异常" , LOG_ERROR: "日志异常" , WRITE_FAILED: "保存失败"};
let token = "" , currentPage = "overview" , recordPage = 1 , recordTotal = 0;
let device = {} , memberList = [] , editing = null , toastTimer , refreshing = false , eventSignature = "";

function esc(value)
{
    return String(value ?? "").replace(/[&<>"']/g , (c) => ({"&": "&amp;" , "<": "&lt;" , ">": "&gt;" , '"': "&quot;" , "'": "&#39;"}[c]));
}

function icon(name)
{
    return `<svg aria-hidden="true"><use href="#i-${name}"/></svg>`;
}

function person(name)
{
    return `<span class="person"><span class="avatar">${esc(Array.from(name || "?")[0].toUpperCase())}</span>${esc(name)}</span>`;
}

function empty(title , hint = "连接终端同步，或导入终端原始 CSV 文件。")
{
    return `<div class="empty">${icon("card")}<strong>${esc(title)}</strong><p>${esc(hint)}</p></div>`;
}

function notify(message , failure = false)
{
    const node = $("#toast");
    node.textContent = message;
    node.classList.toggle("failure" , failure);
    node.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => node.hidden = true , failure ? 8000 : 4500);
}

async function api(path , data , raw = false)
{
    const options = data === undefined ? {} : {method: "POST" , headers: {"X-Admin-Token": token , "Content-Type": raw ? "text/csv" : "application/json"} , body: raw ? data : JSON.stringify(data)};
    const response = await fetch("/api/" + path , options);
    const result = await response.json();
    if (!response.ok)
    {
        throw new Error(result.error || "请求失败");
    }
    return result;
}

async function action(button , task)
{
    if (button.disabled)
    {
        return;
    }
    const label = button.innerHTML;
    button.disabled = true;
    button.textContent = "处理中…";
    try
    {
        await task();
    }
    catch (error)
    {
        notify(error.message , true);
    }
    finally
    {
        button.disabled = false;
        button.innerHTML = label;
    }
}

function renderRecords(rows)
{
    if (!rows.length)
    {
        return empty("该日期范围暂无已收录记录");
    }
    return `<table><thead><tr><th>人员</th><th>卡片 UID</th><th>签到时间</th><th>分组</th><th>状态</th></tr></thead><tbody>${rows.map((r) => `<tr><td>${person(r.name)}</td><td class="mono">${esc(r.uid)}</td><td>${esc(r.time)}</td><td>${esc(r.group_name || "—")}</td><td><span class="badge">已签到</span></td></tr>`).join("")}</tbody></table>`;
}

function renderTrend(data)
{
    const max = Math.max(1 , ...data.map((d) => d.count));
    const ceiling = max <= 4 ? 4 : Math.ceil(max / 4) * 4;
    let svg = `<svg viewBox="0 0 600 210" role="img" aria-label="近七天签到卡片数">`;
    for (let i = 0 ; i <= 4 ; i ++)
    {
        const y = 172 - i * 36;
        svg += `<line x1="37" y1="${y}" x2="587" y2="${y}"/><text x="24" y="${y + 4}" text-anchor="end">${ceiling * i / 4}</text>`;
    }
    data.forEach((d , i) =>
    {
        const height = d.count / ceiling * 144 , x = 65 + i * 77;
        svg += `<rect class="bar ${i === 6 ? "latest" : ""}" x="${x}" y="${172 - height}" width="29" height="${Math.max(height , 2)}" rx="4"><title>${esc(d.day)} · ${d.count} 张卡</title></rect><text x="${x + 14.5}" y="198" text-anchor="middle">${esc(d.day.slice(5).replace("-" , "/"))}</text>`;
        if (d.count)
        {
            svg += `<text class="count" x="${x + 14.5}" y="${162 - height}" text-anchor="middle">${d.count}</text>`;
        }
    });
    $("#trend").innerHTML = svg + "</svg>";
}

async function overview()
{
    const data = await api("overview?day=" + $("#overview-day").value);
    $("#metric-count").innerHTML = data.count + " <small>张卡</small>";
    $("#metric-members").innerHTML = data.registered + " <small>张</small>";
    $("#metric-unseen").innerHTML = data.unseen.length + " <small>张卡</small>";
    $("#metric-total").innerHTML = data.total + " <small>条</small>";
    $("#metric-sync").textContent = data.sync ? "完整同步于 " + data.sync.synced_at.slice(5 , 16) : "该日期尚未完整同步";
    $("#recent-table").innerHTML = renderRecords(data.records);
    renderTrend(data.trend);
}

function recordQuery()
{
    return new URLSearchParams({start: $("#record-start").value , end: $("#record-end").value , q: $("#record-search").value.trim() , page: recordPage});
}

async function records()
{
    const data = await api("records?" + recordQuery());
    recordTotal = data.total;
    $("#records-table").innerHTML = renderRecords(data.rows);
    $("#records-total").textContent = "共 " + data.total + " 条记录";
    $("#page-info").textContent = `第 ${recordPage} / ${Math.max(1 , Math.ceil(data.total / 25))} 页`;
    $("#prev-page").disabled = recordPage <= 1;
    $("#next-page").disabled = recordPage * 25 >= recordTotal;
}

async function members()
{
    memberList = await api("members?q=" + encodeURIComponent($("#member-search").value));
    $("#members-total").textContent = `卡片名单 · ${memberList.length}`;
    $("#members-table").innerHTML = memberList.length ? `<table><thead><tr><th>人员</th><th>卡片 UID</th><th>编号 / 分组</th><th>最近签到</th><th>终端状态</th><th></th></tr></thead><tbody>${memberList.map((m) => `<tr><td>${person(m.name)}</td><td class="mono">${esc(m.uid)}</td><td>${esc([m.student_no , m.group_name].filter(Boolean).join(" / ") || "—")}</td><td>${esc(m.last_checkin || "暂无记录")}</td><td><span class="badge ${m.on_device ? "" : "neutral"}">${m.on_device ? "已确认登记" : "仅本机记录"}</span></td><td><button class="text-button edit-member" data-uid="${esc(m.uid)}">编辑备注</button></td></tr>`).join("")}</tbody></table>` : empty("暂无卡片名单" , "连接终端读取名单，或点击“登记新卡片”。");
    document.querySelectorAll(".edit-member").forEach((b) => b.onclick = () => showMember(b.dataset.uid));
}

async function ports()
{
    const old = $("#port-select").value;
    const data = await api("ports");
    $("#port-select").innerHTML = data.length ? data.map((p) => `<option value="${esc(p.port)}">${esc(p.port)} · ${esc(p.description)}</option>`).join("") : '<option value="">未发现串口，请检查 USB 连接</option>';
    if (data.some((p) => p.port === (device.port || old)))
    {
        $("#port-select").value = device.port || old;
    }
}

function renderState()
{
    const online = device.connected && device.responsive;
    const netMode = !device.connected && window.__netActive === true;
    const headerOnline = netMode ? window.__netOnline === true : online;
    $("#top-connect").classList.toggle("connected" , headerOnline);
    $("#top-connect").innerHTML = `<span class="dot"></span><span>${netMode ? (headerOnline ? "Wi-Fi 终端在线" : "Wi-Fi 终端离线") : online ? esc(device.port) + " 已连接" : device.connected ? "终端无响应" : "终端未连接"}</span>`;
    $("#live-indicator").textContent = online ? "实时接收中" : "等待连接";
    $("#live-indicator").className = "badge" + (online ? "" : " neutral");
    const f = device.fields || {};
    const health = online && f.pn532_state === "READY" && f.sd_state === "MOUNTED" && f.rtc_state === "VALID" && f.registry_ready === "1";
    if (netMode)
    {
        $("#overview-status strong").textContent = headerOnline ? "签到终端已联网" : "等待终端连接热点";
        $("#overview-status p").textContent = headerOnline ? "通过手机热点上传；后台每分钟接收状态，离线历史会自动补传。" : "开启已保存的手机热点，终端将自动重连。离线记录保存在 SD 卡。";
    }
    else
    {
        $("#overview-status strong").textContent = health ? "签到终端运行正常" : online ? "终端已连接，请检查模块状态" : "连接签到终端，开始同步";
        $("#overview-status p").textContent = health ? `${device.port} · 读卡器、时钟与存储已就绪；连接期间自动补齐终端当日记录。` : "离线记录保存在 SD 卡，连接后可按日期导入本机。";
    }
    $("#rtc-text").textContent = online ? (f.rtc_state === "VALID" ? (f.rtc_local || "等待读取时间") + " · 北京时间" : "时钟未就绪，请按电脑时间校时") : "连接后读取 DS3231 时间";
    const labels = [["读卡器 PN532" , f.pn532_state , "READY"] , ["OLED 屏幕" , f.oled_state , "INITIALIZED"] , ["时钟 DS3231" , f.rtc_state , "VALID"] , ["microSD 存储" , f.sd_state , "MOUNTED"] , ["卡片名单" , f.registry_ready === "1" ? "READY" : f.registry_ready , "READY"]];
    const issues = {NEEDS_TIME_SYNC: "需要校时" , MOUNT_FAILED: "挂载失败" , NO_CARD: "未检测到卡" , NO_PN532_RESPONSE: "读卡器无响应" , I2C_READ_FAILED: "时钟读取失败" , "0": "未就绪"};
    $("#device-status").innerHTML = labels.map(([label , value , pass]) => `<div class="status-row"><span>${label}</span><span class="badge ${!online ? "neutral" : value === pass ? "" : "warning"}">${!online ? "等待连接" : value === pass ? "正常" : esc(issues[value] || value || "等待读取")}</span></div>`).join("");
    $("#error-banner").hidden = !device.error;
    $("#error-banner").textContent = device.error || "";
    $("#serial-log").textContent = (device.logs || []).map((r) => r.time + "  " + r.text).join("\n") || "暂无串口输出";
    $("#event-feed").innerHTML = (device.events || []).length ? device.events.slice(0 , 12).map((e) => `<div class="event"><span class="badge ${e.state === "SAVED" ? "" : e.state === "ALREADY_TODAY" ? "neutral" : "warning"}">${esc(states[e.state] || e.state)}</span><span class="event-person">${esc(e.name || "未登记")}</span><span class="mono">${esc(e.uid)}</span><time>${esc(e.time)}</time></div>`).join("") : empty("等待下一次刷卡" , "连接终端后，刷卡结果会自动显示在这里。");
    $("#last-card-hint").textContent = device.last_card ? "最近刷卡 " + device.last_card.uid + " · " + device.last_card.seen_at : "尚未读取到卡片";
    $("#device-sync").disabled = !online;
    $("#clock-button").disabled = !online;
}

async function refreshState()
{
    device = await api("state");
    renderState();
    if (typeof refreshNetwork === "function" && $("#net-devices"))
    {
        await refreshNetwork();
    }
    const signature = JSON.stringify((device.events || [])[0]);
    if (signature !== eventSignature)
    {
        eventSignature = signature;
        await refreshPage();
    }
}

async function refreshPage()
{
    if (currentPage === "overview")
    {
        await overview();
    }
    else if (currentPage === "records")
    {
        await records();
    }
    else if (currentPage === "members")
    {
        await members();
    }
    else if (currentPage === "device" && $("#net-devices") && typeof refreshNetwork === "function")
    {
        await refreshNetwork();
    }
}

async function navigate()
{
    const name = location.hash === "#network" ? "device" : location.hash.slice(1);
    currentPage = pages[name] ? name : "overview";
    document.querySelectorAll(".page").forEach((p) => p.hidden = p.id !== "page-" + currentPage);
    document.querySelectorAll("[data-page]").forEach((p) => p.classList.toggle("active" , p.dataset.page === currentPage));
    $("#crumb").textContent = pages[currentPage];
    document.title = pages[currentPage] + " · 到场";
    try
    {
        await refreshPage();
        if (currentPage === "device")
        {
            await ports();
        }
    }
    catch (error)
    {
        notify(error.message , true);
    }
}

async function syncDay(day)
{
    if (!device.connected && $("#net-devices"))
    {
        await queueRemote("rescan");
        location.hash = "device";
        return;
    }
    const result = await api("device/sync" , {day});
    notify(`已同步 ${day}，新增 ${result.added} 条，已有 ${result.existing} 条。`);
    await refreshPage();
}

function showMember(uid = null)
{
    if (!uid && !device.connected && $("#net-devices"))
    {
        location.hash = "device";
        notify("请在终端管理填写新卡 UID 和姓名，提交登记任务。");
        return;
    }
    editing = uid;
    $("#dialog-error").hidden = true;
    $("#enroll-fields").hidden = !!uid;
    $("#edit-fields").hidden = !uid;
    $("#dialog-title").textContent = uid ? "编辑人员备注" : "登记新卡片";
    $("#member-submit").textContent = uid ? "保存本机备注" : "写入终端并登记";
    if (uid)
    {
        const m = memberList.find((entry) => entry.uid === uid);
        $("#edit-person").textContent = m.name + " / " + m.uid;
        $("#edit-number").value = m.student_no;
        $("#edit-group").value = m.group_name;
        $("#edit-notes").value = m.notes;
    }
    else
    {
        $("#enroll-uid").value = "";
        $("#enroll-name").value = "";
    }
    $("#member-dialog").showModal();
}

function bindActions()
{
    window.addEventListener("hashchange" , navigate);
    $("#top-connect").onclick = () => location.hash = "device";
    $("#overview-day").onchange = () => overview().catch((e) => notify(e.message , true));
    $("#quick-sync").onclick = (e) => action(e.currentTarget , () => syncDay($("#overview-day").value));
    $("#quick-enroll").onclick = () => showMember();
    $("#enroll-button").onclick = () => showMember();
    $("#record-filter").onsubmit = (e) =>
    {
        e.preventDefault();
        recordPage = 1;
        records().catch((err) => notify(err.message , true));
    };
    $("#record-sync").onclick = (e) => action(e.currentTarget , () => syncDay($("#record-end").value));
    $("#device-sync").onclick = (e) => action(e.currentTarget , () => syncDay($("#sync-day").value));
    $("#prev-page").onclick = () => { recordPage --; records().catch((e) => notify(e.message , true)); };
    $("#next-page").onclick = () => { recordPage ++; records().catch((e) => notify(e.message , true)); };
    $("#export-button").onclick = () =>
    {
        if ($("#record-start").value > $("#record-end").value)
        {
            notify("开始日期不能晚于结束日期" , true);
            return;
        }
        location.href = "/api/export?" + recordQuery();
    };
    $("#refresh-ports").onclick = (e) => action(e.currentTarget , ports);
    $("#connect-button").onclick = (e) => action(e.currentTarget , async () =>
    {
        await api("device/connect" , {port: $("#port-select").value});
        await refreshState();
        notify("终端已连接，卡片名单已读取。");
    });
    $("#disconnect-button").onclick = (e) => action(e.currentTarget , async () =>
    {
        await api("device/disconnect" , {});
        await refreshState();
        notify("USB 管理连接已断开，终端可继续独立签到。");
    });
    $("#roster-sync").onclick = (e) => action(e.currentTarget , async () =>
    {
        if (!device.connected && $("#net-devices"))
        {
            await queueRemote("roster");
            return;
        }
        const data = await api("device/members" , {});
        await members();
        notify(`已读取 ${data.count} 张登记卡片。`);
    });
    $("#clock-button").onclick = (e) => action(e.currentTarget , async () =>
    {
        const data = await api("device/clock" , {});
        notify(data.message);
    });
    let searchTimer;
    $("#member-search").oninput = () =>
    {
        clearTimeout(searchTimer);
        searchTimer = setTimeout(() => members().catch((e) => notify(e.message , true)) , 200);
    };
    document.querySelectorAll(".close-button").forEach((b) => b.onclick = () => $("#member-dialog").close());
    $("#use-last-card").onclick = () =>
    {
        if (!device.connected || !device.responsive || !device.last_card)
        {
            notify("请连接终端并刷一次要登记的卡片。" , true);
            return;
        }
        $("#enroll-uid").value = device.last_card.uid;
    };
    $("#member-form").onsubmit = (e) =>
    {
        e.preventDefault();
        action($("#member-submit") , async () =>
        {
            $("#dialog-error").hidden = true;
            try
            {
                if (editing)
                {
                    await api("member/edit" , {uid: editing , student_no: $("#edit-number").value , group_name: $("#edit-group").value , notes: $("#edit-notes").value});
                }
                else
                {
                    await api("device/bind" , {uid: $("#enroll-uid").value , name: $("#enroll-name").value});
                }
                $("#member-dialog").close();
                notify(editing ? "人员备注已保存到本机。" : "终端已确认登记，名单已更新。");
                await refreshPage();
            }
            catch (err)
            {
                $("#dialog-error").textContent = err.message;
                $("#dialog-error").hidden = false;
            }
        });
    };
    $("#import-button").onclick = () => $("#import-file").click();
    $("#import-file").onchange = async (e) =>
    {
        const file = e.target.files[0];
        if (!file)
        {
            return;
        }
        await action($("#import-button") , async () =>
        {
            if (file.size > 2 * 1024 * 1024)
            {
                throw new Error("文件超过 2 MiB");
            }
            const result = await api("import" , file , true);
            notify(`导入完成，新增 ${result.added} 条，已有 ${result.existing} 条。`);
            recordPage = 1;
            await refreshPage();
        });
        e.target.value = "";
    };
}

async function sol()
{
    try
    {
        const bootstrap = await api("bootstrap");
        token = bootstrap.token;
        ["overview-day" , "record-start" , "record-end" , "sync-day"].forEach((id) => $("#" + id).value = bootstrap.today);
        bindActions();
        if (bootstrap.network && typeof initNetwork === "function")
        {
            initNetwork();
        }
        await refreshState();
        await navigate();
        let tick = 0;
        setInterval(async () =>
        {
            if (refreshing)
            {
                return;
            }
            refreshing = true;
            try
            {
                await refreshState();
                tick ++;
                if (tick % 4 === 0)
                {
                    await refreshPage();
                }
            }
            catch (error)
            {
                $("#error-banner").hidden = false;
                $("#error-banner").textContent = "后台连接中断。请确认启动窗口仍在运行，再刷新页面。";
            }
            finally
            {
                refreshing = false;
            }
        } , 2000);
    }
    catch (error)
    {
        $("#error-banner").textContent = "后台启动失败。" + error.message;
        $("#error-banner").hidden = false;
    }
}

document.addEventListener("DOMContentLoaded" , sol , {once: true});
