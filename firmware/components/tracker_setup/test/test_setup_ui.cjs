'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '..', 'setup.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
assert.match(script, /\ninit\(\);\s*$/);

class Element {
    constructor() {
        this.value = ''; this.checked = false; this.disabled = false;
        this.type = 'text'; this.textContent = ''; this.className = '';
        this.children = []; this.listeners = new Map();
        this.classList = {
            add: name => { this.className = [...new Set([...this.className.split(/\s+/), name])].join(' ').trim(); },
            remove: name => { this.className = this.className.split(/\s+/).filter(value => value !== name).join(' '); },
            contains: name => this.className.split(/\s+/).includes(name),
        };
    }
    addEventListener(type, listener) { this.listeners.set(type, listener); }
    append(...children) { this.children.push(...children); }
    replaceChildren(...children) { this.children = children; }
    focus() { this.focused = (this.focused || 0) + 1; }
    scrollIntoView() { this.scrolled = (this.scrolled || 0) + 1; }
    set innerHTML(_) { throw new Error('Untrusted content must never use innerHTML'); }
}

const ids = new Map(), names = new Map();
for (const match of html.matchAll(/<(?:input|select|button|form|div|section|strong|p|dl)\b([^>]*)>/g)) {
    const attributes = Object.fromEntries([...match[1].matchAll(/([\w-]+)="([^"]*)"/g)].map(m => [m[1], m[2]]));
    const element = new Element();
    Object.assign(element, attributes);
    if (attributes.id) ids.set(attributes.id, element);
    if (attributes.name) names.set(attributes.name, element);
}
const element = id => { if (!ids.has(id)) ids.set(id, new Element()); return ids.get(id); };
const form = element('settings');
form.elements = {namedItem: name => names.get(name) || null};
const formIds = new Set([...html.match(/<form\b[\s\S]*?<\/form>/)[0].matchAll(/\bid="([^"]*)"/g)].map(match => match[1]));
form.querySelectorAll = selector => selector === 'button' ? [...ids.values()].filter(e => formIds.has(e.id) && (e.type === 'button' || e.type === 'submit')) : [...names.values()].filter(e => e.type === 'password');
form.checkValidity = form.reportValidity = () => { throw new Error('Wi-Fi save must not validate unrelated form fields'); };
const document = {
    getElementById: element, createElement: () => new Element(),
    querySelectorAll: () => [], documentElement: {lang: 'en'},
    listeners: new Map(), visibilityState: 'visible',
    addEventListener(type, listener) { this.listeners.set(type, listener); },
};
let request;
let respond = async () => { throw new Error('Unexpected HTTP request'); };
let clock = Date.now(), timerId = 0;
const timeouts = new Map(), intervals = new Map(), requests = [];
class Clock extends Date { static now() { return clock; } }
const context = vm.createContext({document, navigator: {language: 'en'}, location: {hostname: '192.168.88.68'}, console, Date: Clock, AbortController,
    setTimeout(fn, delay) { const id = ++timerId; timeouts.set(id, {fn, delay}); return id; },
    clearTimeout(id) { timeouts.delete(id); },
    setInterval(fn, delay) { const id = ++timerId; intervals.set(id, {fn, delay}); return id; },
    confirm: () => true,
    FormData: class {
        entries() { return [...names.entries()].filter(([, el]) => !el.disabled && (el.type !== 'checkbox' || el.checked)).map(([name, el]) => [name, el.value]); }
    },
    fetch: async (url, options) => { request = {url, options}; requests.push(request); return respond(url, options); },
});
vm.runInContext(script.replace(/\ninit\(\);\s*$/, '\n'), context);
vm.runInContext("csrf='test-session'; configLoaded=true;", context);
const plain = value => JSON.parse(JSON.stringify(value));
const value = (name, value) => { names.get(name).value = value; };
const response = (body, ok = true, status = ok ? 200 : 422) => ({ok, status, json: async () => body});
const statusValue = key => {
    const label = vm.runInContext(`t(${JSON.stringify(key)})`, context);
    const children = element('status-list').children;
    return children[children.findIndex(child => child.textContent === label) + 1].textContent;
};

async function main() {
    value('wifi_ssid', 'Home'); value('wifi_password', '');
    value('wifi_secondary_ssid', 'Hotspot'); value('wifi_secondary_password', 'new-hotspot-key');
    value('endpoint', 'not-a-valid-url'); value('device_id', '');
    value('sim_pin', 'unsaved-pin'); value('setup_password', 'unsaved-ap-password');
    value('access_client_secret', 'unsaved-access-secret');
    names.get('clear_wifi_password').checked = true;
    const payload = plain(context.wifiPayload(form.elements));
    assert.deepEqual(payload, {wifi_ssid: 'Home', wifi_password: '', wifi_secondary_ssid: 'Hotspot',
        wifi_secondary_password: 'new-hotspot-key', clear_wifi_password: true, clear_wifi_secondary_password: false});
    assert.equal(element('save-wifi').type, 'button');

    let finishRequest;
    respond = () => new Promise(resolve => { finishRequest = resolve; });
    const save = element('save-wifi').listeners.get('click')();
    assert.equal([...timeouts.values()].some(timer => timer.delay === 60000), true, 'Save waits for bounded scheduler/apply completion, not the shorter status timeout');
    assert.equal(names.get('wifi_password').disabled, true);
    assert.equal(names.get('endpoint').disabled, false);
    assert.equal(request.url, '/api/wifi');
    assert.equal(request.options.method, 'POST');
    assert.equal(request.options.headers['X-CSRF-Token'], 'test-session');
    assert.deepEqual(JSON.parse(request.options.body), payload);
    finishRequest(response({saved: true, connection_requested: true, wifi_ssid: 'Home',
        wifi_secondary_ssid: 'Hotspot', wifi_password_set: false, wifi_secondary_password_set: true}));
    await save;
    for (const name of ['wifi_password', 'wifi_secondary_password']) {
        assert.equal(names.get(name).value, '');
        assert.equal(names.get('clear_' + name).checked, false);
        assert.equal(names.get(name).disabled, false);
    }
    assert.equal(names.get('endpoint').value, 'not-a-valid-url');
    assert.equal(names.get('sim_pin').value, 'unsaved-pin');
    assert.equal(names.get('setup_password').value, 'unsaved-ap-password');
    assert.equal(names.get('access_client_secret').value, 'unsaved-access-secret');
    assert.equal(vm.runInContext('closed', context), false);
    assert.equal(vm.runInContext('config.wifi_secondary_password_set', context), true);

    value('wifi_password', 'invalid');
    respond = async () => response({error: 'Wi-Fi password invalid'}, false);
    await element('save-wifi').listeners.get('click')();
    assert.equal(names.get('wifi_password').value, 'invalid');
    assert.equal(element('wifi-notice').textContent, 'Wi-Fi password invalid');
    assert.equal(element('wifi-notice').focused, 1);
    assert.equal(element('wifi-notice').scrolled, 1);
    assert.equal(names.get('wifi_password').disabled, false);

    const connected = {connected: true, has_ip: true, connecting: false, profile: 2,
        ssid: '<b>Actual router</b>', ip: '10.0.0.7', rssi: -58};
    const translate = key => key;
    let view = context.wifiStatusView(connected, true, translate);
    assert.equal(view.kind, 'connected');
    assert.match(view.title, /secondaryWifi.*<b>Actual router<\/b>/);
    assert.match(view.detail, /10\.0\.0\.7/);
    assert.doesNotMatch(view.title, /Home|Hotspot/);
    context.renderWifi(connected, true);
    assert.match(element('wifi-state').textContent, /<b>Actual router<\/b>/);
    view = context.wifiStatusView({...connected, has_ip: false}, true, translate);
    assert.equal(view.kind, 'connecting');
    assert.equal(view.title, 'wifiWaitingIp');
    assert.doesNotMatch(view.detail, /10\.0\.0\.7/);
    view = context.wifiStatusView({...connected, connected: false, has_ip: false}, true, translate);
    assert.equal(view.kind, 'disconnected');
    assert.doesNotMatch(view.title + view.detail, /Actual router/);
    assert.equal(context.wifiStatusView(connected, false, translate).kind, 'unknown');
    /* A responding receiver without a fix is different from an absent receiver.
     * SIM7600 omits satellite counts before its first fix: never coerce null to 0. */
    const searchingGnss = {requested: true, modem_ready: true, state: 'searching',
        receiver_running: true, receiver_state_known: true, antenna_powered: true,
        antenna_state_known: true, last_reply_age_ms: 1800, search_elapsed_ms: 3600000,
        poll_count: 1800, error_count: 0, details_available: false, detail_age_ms: null,
        power_age_ms: 10000, fix_mode: null, gps_satellites: null,
        glonass_satellites: null, beidou_satellites: null, hdop: null,
        error: '', detail_error: ''};
    let gnssView = context.gnssStatusView(searchingGnss, true, translate);
    const gnssRow = (view, key) => view.rows.find(([label]) => label === key)[1];
    assert.equal(gnssView.kind, 'searching');
    assert.equal(gnssView.title, 'gnssSearching');
    assert.equal(gnssRow(gnssView, 'gnssSearchTime'), '1 h 0 min');
    assert.equal(gnssRow(gnssView, 'gnssReceiver'), 'gnssOnConfirmed');
    assert.equal(gnssRow(gnssView, 'gnssReply'), '1 s');
    assert.equal(gnssRow(gnssView, 'gnssErrors'), '0');
    assert.doesNotMatch(gnssRow(gnssView, 'gnssUsed'), /: 0/);
    assert.match(gnssRow(gnssView, 'gnssUsed'), /GPS: gnssNotReported.*GLONASS: gnssNotReported.*BeiDou: gnssNotReported/);
    assert.equal(gnssRow(gnssView, 'HDOP'), 'gnssNotReported');
    const fixedGnss = {...searchingGnss, state: 'fix', details_available: true,
        detail_age_ms: 1000, fix_mode: 3, gps_satellites: 8, glonass_satellites: 0,
        beidou_satellites: 2, hdop: 0.9};
    gnssView = context.gnssStatusView(fixedGnss, true, translate);
    assert.equal(gnssView.kind, 'fix');
    assert.equal(gnssRow(gnssView, 'gnssUsed'), 'GPS: 8 · GLONASS: 0 · BeiDou: 2');
    assert.equal(gnssRow(gnssView, 'gnssFixMode'), '3D');
    assert.equal(gnssRow(gnssView, 'HDOP'), '0.9');
    gnssView = context.gnssStatusView({...fixedGnss, detail_age_ms: 31000, power_age_ms: 61000}, true, translate);
    assert.doesNotMatch(gnssRow(gnssView, 'gnssUsed'), /: [028]/);
    assert.equal(gnssRow(gnssView, 'gnssReceiver'), 'gnssPowerUnknown');
    assert.equal(context.gnssStatusView({...searchingGnss, last_reply_age_ms: 20000}, true, translate).title, 'gnssNoReply');
    assert.equal(context.gnssStatusView({...searchingGnss, last_reply_age_ms: null}, true, translate).title, 'gnssNoReply');
    assert.equal(context.gnssStatusView({...searchingGnss, state: 'off', requested: false}, true, translate).title, 'gnssOff');
    assert.equal(context.gnssStatusView(searchingGnss, false, translate).title, 'gnssUnknown');
    assert.equal(context.gnssStatusView(null, true, translate).title, 'gnssUnknown');
    context.renderGnss({...searchingGnss, state: 'error', error: '<img src=x onerror=alert(1)>'}, true);
    assert.equal(element('gnss-list').children.some(child => child.textContent === '<img src=x onerror=alert(1)>'), true, 'Diagnostic text must not use HTML');
    vm.runInContext("language='hu';", context);
    context.renderGnss(searchingGnss, true);
    assert.match(element('gnss-state').textContent, /A vevő válaszol.*pozíció keresése/);
    assert.match(element('gnss-satellite-note').textContent, /hiányzó adat nem jelent nulla műholdat/);
    context.renderGnss(searchingGnss, false);
    assert.match(element('gnss-state').textContent, /nem elérhető/);
    assert.equal(element('gnss-list').children.some(child => child.textContent === '1 s'), false, 'Stale page must remove formerly current GPS data');
    vm.runInContext("language='en';", context);
    respond = async () => { throw new Error('Offline'); };
    await context.refresh();
    assert.match(element('wifi-banner').className, /unknown/);
    assert.doesNotMatch(element('wifi-state').textContent, /Actual router/);

    const maliciousSsid = '<img src=x onerror=alert(1)>';
    respond = async () => response({networks: [{ssid: maliciousSsid, rssi: -45, secured: true}]});
    await element('scan-secondary').listeners.get('click')();
    assert.equal(element('networks-secondary').children[1].value, maliciousSsid);
    assert.match(element('networks-secondary').children[1].textContent, /<img src=x/);
    element('networks-secondary').value = maliciousSsid;
    element('networks-secondary').listeners.get('change')();
    assert.equal(names.get('wifi_secondary_ssid').value, maliciousSsid);
    assert.equal(names.get('wifi_ssid').value, 'Home');
    /* Bottom save errors are visible beside that button and preserve entries. */
    vm.runInContext("language='hu';", context);
    value('web_password', 'unsaved-web-secret');
    respond = async () => response({error: 'Access service credentials are required'}, false);
    await form.listeners.get('submit')({preventDefault() {}});
    assert.equal(request.url, '/api/config');
    assert.match(element('save-notice').textContent, /Cloudflare Access Client ID.*Client Secret/);
    assert.equal(element('save-notice').focused, 1);
    assert.equal(element('save-notice').scrolled, 1);
    assert.equal(names.get('web_password').value, 'unsaved-web-secret');
    assert.equal(names.get('sim_pin').value, 'unsaved-pin');
    assert.equal(vm.runInContext('closed', context), false);
    assert.match(html, /<form[^>]+novalidate/);

    respond = async () => response({error: 'Invalid web_password: check value or conflicting clear request'}, false);
    await form.listeners.get('submit')({preventDefault() {}});
    assert.match(element('save-notice').textContent, /Webfelület jelszava.*Ellenőrizd/);
    assert.doesNotMatch(element('save-notice').textContent, /unsaved-web-secret/);
    assert.equal(element('save-notice').focused, 2);
    assert.equal(names.get('web_password').value, 'unsaved-web-secret');

    respond = async () => response({error: 'Primary Wi-Fi password must be empty, 8-63 characters, or a 64-digit hexadecimal key'}, false);
    await element('save-wifi').listeners.get('click')();
    assert.match(element('wifi-notice').textContent, /Elsődleges Wi-Fi-jelszó.*8–63/);
    assert.equal(element('wifi-notice').focused, 2);
    assert.match(element('save-notice').textContent, /Webfelület jelszava/);

    /* A stored partial configuration is a successful save with a warning,
     * not an upload-ready claim or a validation failure. */
    respond = async () => response({saved: true, upload_ready: false,
        missing_fields: ['access_client_id', 'access_client_secret']});
    await form.listeners.get('submit')({preventDefault() {}});
    assert.equal(JSON.parse(request.options.body).web_password, 'unsaved-web-secret');
    assert.equal(vm.runInContext('closed', context), true);
    assert.match(element('closed-summary').textContent, /beállítások mentve/);
    assert.match(element('closed-summary').textContent, /helyadatküldés még nem indul/);
    assert.match(element('closed-summary').textContent, /Cloudflare Access Client ID; Cloudflare Access Client Secret/);
    assert.match(element('closed').className, /warning/);
    assert.equal(element('closed').focused, 1);
    assert.equal(element('closed').scrolled, 1);
    assert.equal(element('reopen').href, '/');
    assert.equal(context.reopenTarget('192.168.4.1', {has_ip: true, ip: '192.168.88.68'}), 'http://192.168.88.68/');
    assert.equal(context.reopenTarget('192.168.4.1', {has_ip: false, ip: '192.168.88.68'}), '');
    assert.equal(context.reopenTarget('192.168.4.1', {has_ip: true, ip: 'javascript:alert(1)'}), '');
    assert.equal(names.get('web_password').value, '');
    vm.runInContext("language='en'; translate();", context);
    assert.match(element('closed-summary').textContent, /Settings saved.*uploads remain disabled/);
    assert.match(context.closingSummary({saved: true, upload_ready: true}), /attempt location uploads/);
    assert.doesNotMatch(context.closingSummary({saved: true, upload_ready: false, missing_fields: ['access_client_secret']}), /Client ID/);
    /* Closing the editor must leave both status and its requests alive. */
    const liveStatus = {configured: true, gnss_fix: false, sim_pin_blocked: false,
        last_fix_ms: 0, last_upload_ms: 0, queue_count: 0, wifi: connected};
    respond = async () => response(liveStatus);
    await context.refresh();
    assert.equal(request.url, '/api/status');
    assert.equal(form.classList.contains('hidden'), true);
    assert.equal(element('main').classList.contains('hidden'), false);
    assert.equal(statusValue('gnss_fix'), 'No');
    assert.match(element('status-freshness').textContent, /Live status/);
    context.lock(true);
    await context.refresh();
    assert.equal(request.url, '/api/status', 'A save must not suppress status requests');
    context.lock(false);

    /* Failed and hung requests must not keep displaying an old GPS No as current. */
    respond = async () => { throw Error('Offline'); };
    await context.refresh();
    assert.match(statusValue('gnss_fix'), /Unknown/);
    assert.match(element('status-freshness').textContent, /out of date.*last received/);
    assert.equal(element('reconnect-status').classList.contains('hidden'), false);
    respond = async () => response({...liveStatus, gnss_fix: true, applying: true});
    await context.refresh();
    assert.equal(statusValue('gnss_fix'), 'Yes');
    assert.match(element('status-freshness').textContent, /Applying settings/);
    assert.equal(element('reconnect-status').classList.contains('hidden'), true);
    respond = (_url, options) => new Promise((_resolve, reject) => options.signal.addEventListener('abort', () => reject(Error('Aborted'))));
    const hangingRefresh = context.refresh();
    const inFlight = requests.length;
    clock += 16000;
    await context.refresh();
    assert.equal(requests.length, inFlight, 'Never overlap status requests');
    assert.match(statusValue('gnss_fix'), /Unknown/);
    [...timeouts.values()].find(timer => timer.delay === 10000).fn();
    await hangingRefresh;
    assert.equal(vm.runInContext('refreshing', context), false, 'Timed out fetch releases the poll');

    /* Startup failure still starts the timer. Recovery fills only untouched fields. */
    vm.runInContext("closed=false; configLoaded=false; csrf=''; statusError=null; editedFields.clear();", context);
    respond = async () => { throw Error('Still offline'); };
    await context.init();
    assert.equal(intervals.size, 1);
    const poll = [...intervals.values()][0];
    assert.equal(poll.delay, 5000);
    assert.equal(element('save-wifi').disabled, true);
    assert.equal(element('reconnect-status').disabled, false, 'Connection recovery remains usable when the form cannot load');
    value('endpoint', 'https://draft.example/unsaved');
    value('sim_pin', 'draft-pin');
    value('access_client_secret', 'draft-secret');
    names.get('bench_mode').checked = false;
    const beforeTyping = requests.length;
    form.listeners.get('input')({target: names.get('endpoint')});
    form.listeners.get('change')({target: names.get('bench_mode')});
    assert.equal(requests.length, beforeTyping, 'Typing/checking fields must not send activity requests');
    const savedConfig = {csrf_token: 'renewed-session', endpoint: 'https://saved.example/ingest',
        wifi_ssid: 'Saved Wi-Fi', bench_mode: true, access_client_secret_set: true};
    respond = async url => response(url === '/api/config' ? savedConfig : liveStatus);
    await element('reconnect-status').listeners.get('click')();
    assert.deepEqual(requests.slice(-3).map(r => r.url), ['/', '/api/config', '/api/status']);
    assert.equal(names.get('wifi_ssid').value, 'Saved Wi-Fi');
    assert.equal(names.get('endpoint').value, 'https://draft.example/unsaved');
    assert.equal(names.get('bench_mode').checked, false);
    assert.equal(names.get('sim_pin').value, 'draft-pin');
    assert.equal(names.get('access_client_secret').value, 'draft-secret');
    assert.equal(element('save-wifi').disabled, false);
    names.get('retry_sim_pin').checked = true;
    clock += 21 * 60 * 1000;

    /* A long-open page renews an expired cookie/CSRF once, then retries status.
     * Renewed config is metadata only: even unchecked drafts/secrets stay exact. */
    let statusCalls = 0;
    respond = async url => {
        if (url === '/api/status' && ++statusCalls === 1)
            return response({error: 'Open the setup page to start a session'}, false, 403);
        return response(url === '/api/config' ? {...savedConfig, csrf_token: 'next-session', wifi_ssid: 'Changed elsewhere'} : liveStatus);
    };
    const beforeRenewal = requests.length;
    await poll.fn();
    assert.deepEqual(requests.slice(beforeRenewal).map(r => r.url), ['/api/status', '/', '/api/config', '/api/status']);
    assert.equal(vm.runInContext('csrf', context), 'next-session');
    assert.equal(statusValue('gnss_fix'), 'No');
    assert.equal(names.get('wifi_ssid').value, 'Saved Wi-Fi');
    assert.equal(names.get('endpoint').value, 'https://draft.example/unsaved');
    assert.equal(names.get('bench_mode').checked, false);
    assert.equal(names.get('retry_sim_pin').checked, true, 'Status must preserve a draft PIN-retry selection');
    assert.equal(names.get('sim_pin').value, 'draft-pin');
    assert.equal(names.get('access_client_secret').value, 'draft-secret');

    /* A failed renewal is bounded; ordinary status polling continues without
     * repeated page/config requests. Basic login requires an explicit reconnect. */
    respond = async () => response({error: 'Open the setup page to start a session'}, false, 403);
    const beforeFailedRenewal = requests.length;
    await poll.fn();
    await poll.fn();
    assert.deepEqual(requests.slice(beforeFailedRenewal).map(r => r.url), ['/api/status', '/', '/api/status']);
    assert.match(statusValue('gnss_fix'), /Unknown/);
    assert.match(element('status-freshness').textContent, /Reconnect/);
    respond = async () => response({error: 'Web login required (username: admin)'}, false, 401);
    await poll.fn();
    const afterLoginFailure = requests.length;
    await poll.fn();
    assert.equal(requests.length, afterLoginFailure, 'Do not loop Basic login requests');
    respond = async url => response(url === '/api/config' ? savedConfig : liveStatus);
    await element('reconnect-status').listeners.get('click')();
    assert.equal(statusValue('gnss_fix'), 'No');

    /* Intentional save retries only a definite session rejection, never an
     * uncertain timeout/commit, Basic login, Host/Origin failure or server error. */
    let posts = 0;
    const originalDraft = {endpoint: names.get('endpoint').value, secret: names.get('access_client_secret').value};
    respond = async (url, options) => {
        if (options.method === 'POST' && ++posts === 1)
            return response({error: 'Invalid setup session token'}, false, 403);
        return response(url === '/api/config' ? savedConfig : {saved: true});
    };
    const beforeSaveRenewal = requests.length;
    await context.mutate('/api/wifi', {wifi_ssid: 'User saved this'});
    assert.deepEqual(requests.slice(beforeSaveRenewal).map(r => r.url), ['/api/wifi', '/', '/api/config', '/api/wifi']);
    assert.equal(request.options.headers['X-CSRF-Token'], 'renewed-session');
    assert.equal(requests[beforeSaveRenewal].options.body, request.options.body);
    assert.equal(names.get('endpoint').value, originalDraft.endpoint);
    assert.equal(names.get('access_client_secret').value, originalDraft.secret);
    for (const [status, error] of [[401, 'Web login required (username: admin)'], [403, 'Invalid setup origin'], [403, 'Invalid setup host'], [500, 'Internal error']]) {
        respond = async () => response({error}, false, status);
        const before = requests.length;
        await assert.rejects(context.mutate('/api/wifi', {}));
        assert.equal(requests.length, before + 1);
    }
    respond = async () => { throw Error('Connection lost after possible commit'); };
    const beforeAmbiguousSave = requests.length;
    await assert.rejects(context.mutate('/api/wifi', {}), /save result could not be confirmed/);
    assert.equal(requests.length, beforeAmbiguousSave + 1);
    respond = async () => response({error: 'Invalid setup session token'}, false, 403);
    const beforeRepeatedRejection = requests.length;
    await assert.rejects(context.mutate('/api/wifi', {}));
    assert.equal(requests.length, beforeRepeatedRejection + 2, 'A failed renewal must not recurse');
    assert.equal(requests.some(r => r.url === '/api/activity'), false);
    assert.equal(requests.every(r => r.options.credentials === 'same-origin' && r.options.cache === 'no-store'), true);
    console.log('setup UI: Wi-Fi isolation, scan/errors, partial-save feedback, continuous status, GNSS diagnostics/unknown satellites/stale receiver, bounded session recovery and draft preservation passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
