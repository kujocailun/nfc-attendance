"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

const source = process.env.NFC_STATIC_DIR || path.join(__dirname , ".." , "static");
const scripts = ["app.js" , "network.js"].map((name) => ({name , code: fs.readFileSync(path.join(source , name) , "utf8")}));
const selectors = ["#top-connect" , "#live-indicator" , "#overview-status strong" , "#overview-status p" , "#rtc-text" , "#device-status" , "#error-banner" , "#serial-log" , "#event-feed" , "#last-card-hint" , "#net-devices" , "#net-device" , "#net-bind" , "#net-commands" , "#device-sync" , "#clock-button" , "#net-operation-hint"];

function deferred()
{
    let resolve , reject;
    const promise = new Promise((done , fail) =>
    {
        resolve = done;
        reject = fail;
    });
    return {promise , resolve , reject};
}

function browser(usb , devices)
{
    const writes = [];
    const nodes = new Map(selectors.map((selector) =>
    {
        const classes = new Set();
        const node = {value: "" , classList:
        {
            toggle(name , enabled)
            {
                enabled ? classes.add(name) : classes.delete(name);
            } ,
            contains(name)
            {
                return classes.has(name);
            }
        }};
        for (const property of ["innerHTML" , "textContent"])
        {
            let value = "";
            Object.defineProperty(node , property ,
            {
                get: () => value ,
                set(next)
                {
                    value = next;
                    writes.push({selector , property , value: next});
                }
            });
        }
        return [selector , node];
    }));
    let held = null;
    const buttons = ["clock" , "roster" , "rescan"].map((kind) => ({dataset: {kind}}));
    let commands = [];
    const response = (data) => ({ok: true , json: async () => data});
    const context = vm.createContext(
    {
        window: {} , location: {hash: "#device"} ,
        document:
        {
            addEventListener: () => {} ,
            querySelector(selector)
            {
                assert.ok(nodes.has(selector) , "Unexpected selector: " + selector);
                return nodes.get(selector);
            } ,
            querySelectorAll: (selector) => selector === ".remote-operation" ? buttons : []
        } ,
        fetch: async (url) =>
        {
            if (url === "/api/bootstrap")
            {
                // Keep automatic page startup pending; exercise actual refresh functions below.
                return new Promise(() => {});
            }
            if (url === "/api/state")
            {
                return response(usb);
            }
            assert.equal(url , "/api/network");
            if (held)
            {
                const current = held;
                held = null;
                current.requested.resolve();
                await current.release.promise;
                return response({devices: current.devices , commands});
            }
            return response({devices , commands});
        }
    });
    for (const script of scripts)
    {
        vm.runInContext(script.code , context , {filename: script.name});
    }
    vm.runInContext('currentPage = "guide"; eventSignature = undefined;' , context);
    return {
        writes ,
        node: (selector) => nodes.get(selector) ,
        buttons ,
        setUSB: (value) => usb = value ,
        setCommands: (value) => commands = value ,
        refresh: () => vm.runInContext("refreshState()" , context) ,
        delay(nextDevices = devices)
        {
            held = {devices: nextDevices , requested: deferred() , release: deferred()};
            return held;
        }
    };
}

const disconnected = {connected: false , responsive: false};
const wifi = (online) => ({id: "s3-test" , label: "Test terminal" , online , revoked: false , telemetry: {}});
const usb = {connected: true , responsive: true , port: "COM7" , fields: {pn532_state: "READY" , sd_state: "MOUNTED" , rtc_state: "VALID" , registry_ready: "1"}};

test("Fast bootstrap waits for both deferred scripts before initializing network management" , async () =>
{
    const listeners = new Map();
    let initialized = 0;
    const context = vm.createContext(
    {
        window: {} , location: {hash: "#device"} , setInterval: () => {} ,
        document:
        {
            addEventListener: (name , callback) => listeners.set(name , callback) ,
            querySelector: () => ({})
        } ,
        fetch: async () => ({ok: true , json: async () => ({network: true , token: "test" , today: "2026-11-08"})})
    });
    vm.runInContext(scripts[0].code , context , {filename: scripts[0].name});
    vm.runInContext("bindActions = () => {}; refreshState = async () => {}; navigate = async () => {};" , context);
    // Let a cached bootstrap finish before the second deferred script is available.
    await new Promise((resolve) => setImmediate(resolve));
    vm.runInContext(scripts[1].code , context , {filename: scripts[1].name});
    context.initNetwork = () => initialized ++;
    const ready = listeners.get("DOMContentLoaded");
    if (ready)
    {
        await ready();
    }
    assert.equal(initialized , 1);
});

for (const online of [true , false])
{
    test("Wi-Fi " + (online ? "online" : "offline") + " stays stable while network refresh is delayed" , async () =>
    {
        const page = browser(disconnected , [wifi(online)]);
        await page.refresh();
        const expected = page.node("#top-connect").innerHTML;
        assert.ok(expected.includes(online ? "Wi-Fi 终端在线" : "Wi-Fi 终端离线"));
        const banner = page.node("#overview-status strong").textContent;
        const hint = page.node("#overview-status p").textContent;
        page.writes.length = 0;
        const gate = page.delay();
        const refresh = page.refresh();
        await gate.requested.promise;
        assert.equal(page.node("#top-connect").innerHTML , expected);
        assert.equal(page.node("#overview-status strong").textContent , banner);
        assert.equal(page.node("#overview-status p").textContent , hint);
        gate.release.resolve();
        await refresh;
        assert.equal(page.node("#top-connect").classList.contains("connected") , online);
        for (const write of page.writes)
        {
            if (write.selector === "#top-connect")
            {
                assert.equal(write.value , expected);
            }
            if (write.selector === "#overview-status strong")
            {
                assert.equal(write.value , banner);
            }
            if (write.selector === "#overview-status p")
            {
                assert.equal(write.value , hint);
            }
        }
    });
}

test("Wi-Fi changes to offline only when the new network response arrives" , async () =>
{
    const page = browser(disconnected , [wifi(true)]);
    await page.refresh();
    const gate = page.delay([wifi(false)]);
    const refresh = page.refresh();
    await gate.requested.promise;
    assert.ok(page.node("#top-connect").innerHTML.includes("Wi-Fi 终端在线"));
    gate.release.resolve();
    await refresh;
    assert.ok(page.node("#top-connect").innerHTML.includes("Wi-Fi 终端离线"));
    assert.equal(page.node("#overview-status strong").textContent , "等待终端连接热点");
    assert.equal(page.node("#top-connect").classList.contains("connected") , false);
});

test("USB management stays visible when a paired Wi-Fi terminal exists" , async () =>
{
    const page = browser(usb , [wifi(true)]);
    await page.refresh();
    const gate = page.delay();
    const refresh = page.refresh();
    await gate.requested.promise;
    assert.ok(page.node("#top-connect").innerHTML.includes("COM7 已连接"));
    assert.equal(page.node("#overview-status strong").textContent , "签到终端运行正常");
    gate.release.resolve();
    await refresh;
    assert.ok(page.node("#top-connect").innerHTML.includes("COM7 已连接"));
    assert.equal(page.node("#top-connect").classList.contains("connected") , true);
});

test("An unresponsive USB connection remains visible" , async () =>
{
    const page = browser({...usb , responsive: false} , [wifi(true)]);
    await page.refresh();
    await page.refresh();
    assert.ok(page.node("#top-connect").innerHTML.includes("终端无响应"));
    assert.equal(page.node("#top-connect").classList.contains("connected") , false);
});

for (const devices of [[] , [{...wifi(true) , revoked: true}]])
{
    test((devices.length ? "Revoked" : "Absent") + " Wi-Fi devices do not replace USB status" , async () =>
    {
        const page = browser(disconnected , devices);
        await page.refresh();
        await page.refresh();
        assert.ok(page.node("#top-connect").innerHTML.includes("终端未连接"));
        assert.equal(page.node("#top-connect").classList.contains("connected") , false);
        assert.equal(page.node("#overview-status strong").textContent , "连接签到终端，开始同步");
    });
}

test("A failed network request does not replace the cached header with USB status" , async () =>
{
    const page = browser(disconnected , [wifi(true)]);
    await page.refresh();
    const expected = page.node("#top-connect").innerHTML;
    const banner = page.node("#overview-status strong").textContent;
    const gate = page.delay();
    const refresh = page.refresh();
    const rejected = assert.rejects(refresh , /Network unavailable/);
    await gate.requested.promise;
    gate.release.reject(new Error("Network unavailable"));
    await rejected;
    assert.equal(page.node("#top-connect").innerHTML , expected);
    assert.equal(page.node("#overview-status strong").textContent , banner);
});

test("A new USB connection takes precedence before a delayed Wi-Fi response" , async () =>
{
    const page = browser(disconnected , [wifi(true)]);
    await page.refresh();
    page.setUSB(usb);
    const gate = page.delay();
    const refresh = page.refresh();
    await gate.requested.promise;
    assert.ok(page.node("#top-connect").innerHTML.includes("COM7 已连接"));
    gate.release.resolve();
    await refresh;
    assert.ok(page.node("#top-connect").innerHTML.includes("COM7 已连接"));
});

test("Revoking a cached Wi-Fi terminal restores disconnected status immediately" , async () =>
{
    const page = browser(disconnected , [wifi(true)]);
    await page.refresh();
    const gate = page.delay([{...wifi(true) , revoked: true}]);
    const refresh = page.refresh();
    await gate.requested.promise;
    gate.release.resolve();
    await refresh;
    assert.ok(page.node("#top-connect").innerHTML.includes("终端未连接"));
});

test("Remote operation buttons require capabilities and prevent duplicate pending work" , async () =>
{
    const terminal = wifi(false);
    const page = browser(disconnected , [terminal]);
    await page.refresh();
    assert.ok(page.buttons.every((button) => button.disabled));
    terminal.capabilities = ["clock" , "roster" , "rescan"];
    await page.refresh();
    assert.ok(page.buttons.every((button) => !button.disabled));
    page.setCommands([{id: "1" , device_id: terminal.id , kind: "clock" , state: "running" , payload: "{}" , created_at: "now"}]);
    await page.refresh();
    assert.equal(page.buttons[0].disabled , true);
    assert.equal(page.buttons[1].disabled , false);
    assert.ok(page.node("#net-commands").innerHTML.includes("已下发，等待确认"));
    assert.ok(!page.node("#net-commands").innerHTML.includes("undefined"));
});
