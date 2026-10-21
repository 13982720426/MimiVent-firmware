/*
  项目：LitterBoxVentilation
  ESP32-C3 单 PIR 猫砂盆自动排风 + 手机网页面板（SoftAP）

  当前页面通过 ESP32 自建热点访问：VentBox / litterbox123
  手机连接热点后打开 http://192.168.4.1
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>
#include "StatusLed.h"

// ===== 硬件配置 =====
constexpr uint8_t PIR_PIN = 4;
constexpr uint8_t RELAY_PIN = 5;
constexpr bool RELAY_ACTIVE_LOW = false;  // 已实测：GPIO 高电平时继电器吸合

// ===== 调试/正式时序 =====
// true：PIR LOW 后延时 10 秒、风机最短运行 10 秒，方便台架验证。
// false：PIR LOW 后延时 3 分钟、风机最短运行 30 秒。
constexpr bool DEMO_MODE = true;
constexpr unsigned long PIR_POLL_INTERVAL_MS = DEMO_MODE ? 10UL : 50UL;
constexpr unsigned long EXHAUST_DELAY_MS = DEMO_MODE ? 10UL * 1000UL : 3UL * 60UL * 1000UL;
constexpr unsigned long FAN_MIN_RUN_MS = DEMO_MODE ? 10UL * 1000UL : 30UL * 1000UL;

// ===== Wi-Fi 热点配置（修改这里即可更换热点名和密码） =====
// 密码需至少 8 个字符；当前按你的要求，两者使用相同值。
constexpr char AP_SSID[] = "VentBox88";
constexpr char AP_PASSWORD[] = "VentBox88";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

WebServer server(80);

enum class ControlMode : uint8_t {
  AUTO,
  TIMED_ON,
  TIMED_OFF,
  LOCKED_OFF,
};

ControlMode controlMode = ControlMode::AUTO;
bool fanOn = false;
bool presenceDetected = false;
bool previousPresenceDetected = false;
bool suppressPresenceUntilLow = false;
unsigned long lastPresenceMs = 0;
unsigned long fanStartedMs = 0;
unsigned long overrideStartedMs = 0;
unsigned long overrideDurationMs = 0;
unsigned long lockOffArmedMs = 0;
bool lockOffArmed = false;
unsigned long lastPirPollMs = 0;
unsigned long lastLedUpdateMs = 0;

// 手机端单页控制台，使用原生 CSS/JS，不依赖外部 CDN，断网时也能完整加载。
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
  <meta name="theme-color" content="#101a18"><title>VentBox · 猫砂盆新风</title>
  <style>
    :root{color-scheme:dark;--bg:#101a18;--panel:#192522;--line:#2b3b36;--muted:#9aada5;--text:#eef5f0;--mint:#a7f3d0;--amber:#ffc56d;--red:#ff8f83}
    *{box-sizing:border-box}body{margin:0;background:radial-gradient(ellipse at 85% -10%,#315147 0,transparent 38%),var(--bg);color:var(--text);font:15px/1.45 -apple-system,BlinkMacSystemFont,"SF Pro Display","Segoe UI",sans-serif;min-height:100vh}
    .shell{max-width:720px;margin:auto;padding:24px 18px calc(30px + env(safe-area-inset-bottom))}.top{display:flex;justify-content:space-between;align-items:center;margin:4px 0 28px}.brand{display:flex;gap:11px;align-items:center}.mark{width:40px;height:40px;border-radius:14px;background:#b8f5d8;color:#14221d;display:grid;place-items:center;font-size:21px;font-weight:900;box-shadow:0 8px 26px #a7f3d022}.eyebrow{color:var(--muted);font-size:11px;letter-spacing:.14em;text-transform:uppercase}.brand h1{font-size:17px;margin:1px 0 0;letter-spacing:-.03em}.connection{border:1px solid var(--line);border-radius:99px;padding:7px 10px;color:var(--muted);font-size:11px;display:flex;gap:7px;align-items:center}.dot{width:7px;height:7px;border-radius:50%;background:var(--mint);box-shadow:0 0 12px #a7f3d088}.hero{padding:23px;border:1px solid #354740;border-radius:26px;background:linear-gradient(145deg,#23352f,#18231f 70%);position:relative;overflow:hidden;box-shadow:0 20px 55px #0003}.hero:after{content:"";position:absolute;width:200px;height:200px;right:-70px;top:-90px;border:1px solid #ffffff0b;border-radius:50%;box-shadow:0 0 0 25px #ffffff05,0 0 0 52px #ffffff03}.hero-head{display:flex;justify-content:space-between;align-items:start;position:relative;z-index:1}.label{font-size:12px;color:var(--muted);letter-spacing:.08em}.hero h2{font-size:28px;letter-spacing:-.05em;margin:7px 0 3px}.sub{font-size:13px;color:var(--muted)}.fan-orb{width:76px;height:76px;border-radius:50%;display:grid;place-items:center;background:#b8f5d817;border:1px solid #b8f5d844;color:var(--mint);font-size:31px}.fan-orb.on{color:var(--amber);background:#ffc56d18;border-color:#ffc56d66;animation:spin 2s linear infinite}@keyframes spin{to{transform:rotate(360deg)}}.hero-foot{display:flex;align-items:center;gap:8px;margin-top:23px;position:relative;z-index:1}.pill{border:1px solid #ffffff14;background:#ffffff08;padding:6px 10px;border-radius:99px;font-size:11px;color:#d7e3dc}.pill.active{color:var(--mint);border-color:#a7f3d044}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:13px}.card{border:1px solid #2b3b36;border-radius:20px;background:#17221f;padding:16px;cursor:default;box-shadow:inset 0 1px #ffffff04}.card-head{display:flex;justify-content:space-between;align-items:center;color:var(--muted);font-size:12px}.icon{font-size:17px}.value{font-size:19px;font-weight:650;margin-top:13px;letter-spacing:-.03em}.hint{font-size:11px;color:var(--muted);margin-top:4px}.section{margin-top:26px}.section-title{display:flex;justify-content:space-between;align-items:end;margin:0 2px 12px}.section-title h3{font-size:15px;margin:0}.section-title span{font-size:11px;color:var(--muted)}.duration-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}.duration{border:1px solid #354740;border-radius:14px;background:#1b2a25;color:#d7e3dc;padding:11px 4px;font-size:12px;font-weight:650;cursor:pointer;transition:.18s;box-shadow:0 2px 0 #0b1210}.duration small{display:block;color:var(--muted);font-size:10px;font-weight:400;margin-top:3px}.duration:hover{border-color:#ffc56d;color:#ffc56d;transform:translateY(-1px)}.duration.selected{border-color:#ffc56d;color:#ffc56d;background:#ffc56d12;box-shadow:0 5px 18px #ffc56d0d}.duration:active,.danger-button:active,.confirm-actions button:active,.resume-button:active{transform:scale(.98)}.danger-card{display:flex;justify-content:space-between;align-items:center;gap:14px;border:1px solid #793d39;border-radius:18px;background:linear-gradient(120deg,#381f20,#241b1c);padding:15px;cursor:default}.danger-title{font-size:13px;font-weight:700;color:#ffd8d3}.danger-copy{font-size:11px;color:#bd9692;margin-top:4px}.danger-button{border:1px solid #ff8f83;background:#b4423b;color:white;border-radius:12px;padding:12px 13px;font-weight:750;white-space:nowrap;cursor:pointer;box-shadow:0 3px 0 #6c2926}.danger-button:hover,.confirm-danger:hover{background:#d55048;box-shadow:0 5px 16px #ff8f8330}.footer{display:flex;justify-content:space-between;color:#73877e;font-size:10px;margin:24px 3px 0}.toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,20px);opacity:0;pointer-events:none;background:#e6f8ed;color:#15241d;padding:10px 15px;border-radius:99px;font-size:12px;box-shadow:0 8px 30px #0005;transition:.2s}.toast.show{opacity:1;transform:translate(-50%,0)}.confirm-backdrop{position:fixed;inset:0;background:#050907bd;display:none;place-items:center;padding:20px;z-index:5}.confirm-backdrop.open{display:grid}.confirm-box{width:min(100%,410px);border:1px solid #70403c;border-radius:22px;background:#211b1b;padding:23px;box-shadow:0 24px 80px #0009}.confirm-icon{width:42px;height:42px;border-radius:14px;background:#ff8f8324;color:#ff9d92;display:grid;place-items:center;font-size:21px}.confirm-box h3{margin:16px 0 7px;font-size:19px}.confirm-box p{margin:0;color:#bda8a5;font-size:13px;line-height:1.6}.confirm-actions{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:20px}.confirm-actions button{border-radius:13px;padding:13px;border:1px solid var(--line);font-weight:700;cursor:pointer}.cancel{background:#29332f;color:#dce8e1}.confirm-danger{background:#b4423b;border-color:#ff8f83!important;color:white}
    .resume-button{width:100%;margin-top:0;border:1px solid #8cd8b4;border-radius:17px;background:linear-gradient(115deg,#b8f5d8,#89dfb5);color:#14221d;padding:15px 17px;display:flex;align-items:center;justify-content:space-between;text-align:left;box-shadow:0 10px 28px #a7f3d01c;cursor:pointer;transition:.18s}.resume-button span{display:flex;flex-direction:column;gap:3px}.resume-button b{font-size:15px}.resume-button small{font-size:11px;color:#34594a}.resume-button i{font-style:normal;font-size:21px}.resume-button.selected{background:linear-gradient(115deg,#d0fae5,#a7f3d0);color:#123c2c;border-color:#72d6a6;box-shadow:0 0 0 2px #a7f3d022,0 8px 22px #a7f3d01c}.resume-button:hover{filter:brightness(1.04);transform:translateY(-1px)}.resume-button.selected small{color:#34594a}
    .credentials{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:13px;padding:14px 15px;border:1px solid #25352f;border-radius:15px;background:#141e1b}.credentials div{display:flex;flex-direction:column;gap:3px}.credentials span{font-size:10px;color:#83968d}.credentials strong{font-size:12px;letter-spacing:.04em;color:#c6d8ce;font-weight:650}
    @media(min-width:600px){.shell{padding-top:38px}.hero{padding:28px}.hero h2{font-size:32px}.grid{gap:14px}.card{padding:19px}}
  </style>
</head>
<body><main class="shell">
  <header class="top"><div class="brand"><div class="mark">V</div><div><div class="eyebrow">HOME AIR SYSTEM</div><h1>猫砂盆新风控制台</h1></div></div><div class="connection"><i class="dot"></i><span id="net">设备在线</span></div></header>
  <section class="hero"><div class="hero-head"><div><div class="label">EXHAUST STATUS</div><h2 id="fanTitle">风机待机</h2><div class="sub" id="fanSub">等待传感器触发</div></div><div class="fan-orb" id="fanOrb">✳</div></div><div class="hero-foot"><span class="pill active" id="modePill">自动运行</span><span class="pill" id="delayPill">延时排风 10 秒</span></div></section>
  <section class="grid"><article class="card"><div class="card-head"><span>空间感应</span><span class="icon">⌁</span></div><div class="value" id="pirValue">等待中</div><div class="hint" id="pirHint">PIR · GPIO 4</div></article><article class="card"><div class="card-head"><span>风机运行</span><span class="icon">◌</span></div><div class="value" id="runtime">00:00</div><div class="hint" id="runHint">本次运行时长</div></article></section>
  <section class="section auto-section"><button class="resume-button selected" data-auto aria-pressed="true"><span><b id="autoLabel">✓　已启用自动感应</b><small id="autoHint">检测到活动时开启；离开后延时关闭</small></span><i>→</i></button></section>
  <section class="section"><div class="section-title"><h3>临时排风</h3><span>到时自动恢复感应</span></div><div class="duration-grid"><button class="duration" data-timed="on" data-seconds="30">30 秒<small>快速换气</small></button><button class="duration" data-timed="on" data-seconds="60">1 分钟<small>短时排风</small></button><button class="duration" data-timed="on" data-seconds="300">5 分钟<small>常规清新</small></button><button class="duration" data-timed="on" data-seconds="600">10 分钟<small>加强排风</small></button></div></section>
  <section class="section"><div class="section-title"><h3>临时暂停</h3><span>暂停后自动恢复感应</span></div><div class="duration-grid"><button class="duration" data-timed="off" data-seconds="30">30 秒<small>短暂停止</small></button><button class="duration" data-timed="off" data-seconds="60">1 分钟<small>临时静音</small></button><button class="duration" data-timed="off" data-seconds="300">5 分钟<small>清理维护</small></button><button class="duration" data-timed="off" data-seconds="600">10 分钟<small>较长维护</small></button></div></section>
  <section class="section"><div class="danger-card"><div><div class="danger-title">持续关闭 · 高优先级</div><div class="danger-copy">忽略 PIR，直到手动恢复自动或设备重启</div></div><button class="danger-button" id="lockOff">持续关闭…</button></div></section>
  <footer class="footer"><span>VentBox · ESP32-C3</span><span>控制地址　192.168.4.1</span></footer>
  <section class="credentials" aria-label="Wi-Fi 连接信息"><div><span>Wi-Fi 名称</span><strong>VentBox88</strong></div><div><span>Wi-Fi 密码</span><strong>VentBox88</strong></div></section>
</main><div class="toast" id="toast"></div>
<div class="confirm-backdrop" id="confirmBackdrop"><section class="confirm-box" role="dialog" aria-modal="true" aria-labelledby="confirmTitle"><div class="confirm-icon">!</div><h3 id="confirmTitle">确认持续关闭？</h3><p>启用后，PIR 感应会被忽略，风机将一直保持关闭。只有手动恢复自动模式或重启设备才会解除。请确认猫砂盆暂时不需要自动排风。</p><div class="confirm-actions"><button class="cancel" id="cancelLock">取消</button><button class="confirm-danger" id="confirmLock">确认持续关闭</button></div></section></div>
<script>
const $=s=>document.querySelector(s);let busy=false,toastTimer;
function toast(t){const e=$('#toast');e.textContent=t;e.classList.add('show');clearTimeout(toastTimer);toastTimer=setTimeout(()=>e.classList.remove('show'),1900)}
async function request(path,body){const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok)throw Error(await r.text());return r.json()}
async function timed(mode,seconds){if(busy)return;busy=true;try{await request('/api/timed',`mode=${mode}&seconds=${seconds}`);await refresh();toast(`${mode==='on'?'定时排风':'临时暂停'}已启动，将于到时自动恢复感应`)}catch{toast('操作失败，请检查设备连接')}finally{busy=false}}
document.querySelectorAll('[data-timed]').forEach(b=>b.addEventListener('click',()=>timed(b.dataset.timed,b.dataset.seconds)));
async function setAuto(){if(busy)return;busy=true;try{await request('/api/auto','');await refresh();toast('风机已立即停止，自动感应已恢复')}catch{toast('操作失败，请检查设备连接')}finally{busy=false}}
function fmt(ms){let s=Math.ceil(ms/1000);return String(Math.floor(s/60)).padStart(2,'0')+':'+String(s%60).padStart(2,'0')}
function pretty(ms){let s=Math.ceil(ms/1000);return s>=60?`${Math.floor(s/60)}分${s%60?`${s%60}秒`:''}`:`${s}秒`}
async function armLock(){if(busy)return;busy=true;try{await request('/api/lockoff','stage=prepare');$('#confirmBackdrop').classList.add('open')}catch{toast('无法进入确认，请检查连接')}finally{busy=false}}
async function confirmLock(){if(busy)return;busy=true;try{await request('/api/lockoff','stage=confirm');$('#confirmBackdrop').classList.remove('open');await refresh();toast('持续关闭已启用')}catch{$('#confirmBackdrop').classList.remove('open');toast('确认已超时，请重新操作')}finally{busy=false}}
async function cancelLock(){try{await request('/api/lockoff','stage=cancel')}catch{}$('#confirmBackdrop').classList.remove('open')}
$('#lockOff').addEventListener('click',armLock);$('#confirmLock').addEventListener('click',confirmLock);$('#cancelLock').addEventListener('click',cancelLock);$('#confirmBackdrop').addEventListener('click',e=>{if(e.target.id==='confirmBackdrop')cancelLock()});
async function refresh() {
  try {
    const response = await fetch('/api/status', { cache: 'no-store' });
    if (!response.ok) throw Error();
    const d = await response.json();
    const isAuto = d.mode === 'auto';
    const autoButton = $('[data-auto]');

    $('#net').textContent = '设备在线';
    $('#net').style.color = '';
    $('#fanTitle').textContent = d.fan ? '风机运行中' : '风机待机';
    $('#fanSub').textContent = d.fan
      ? (d.mode === 'timed_on' ? '定时强制排风' : d.mode === 'lock_off' ? '持续关闭锁定' : d.pir ? '检测到活动' : d.mode === 'timed_off' ? '临时暂停中' : '延时排风中')
      : d.suppressed ? '当前触发已清除，等待 PIR 复位' : '等待传感器触发';
    $('#fanOrb').classList.toggle('on', d.fan);
    $('#pirValue').textContent = d.pir ? '检测到活动' : '环境稳定';
    $('#pirHint').textContent = d.pir ? 'PIR · GPIO 4 · HIGH' : 'PIR · GPIO 4 · LOW';
    $('#runtime').textContent = d.fan ? fmt(d.runMs) : '00:00';
    $('#runHint').textContent = d.mode.startsWith('timed_')
      ? '自动恢复倒计时 ' + pretty(d.remaining)
      : d.mode === 'lock_off' ? '持续关闭锁定'
      : d.fan && !d.pir ? '延时停止倒计时 ' + fmt(d.remaining) : '本次运行时长';

    $('#modePill').textContent = isAuto ? '自动感应' : d.mode === 'timed_on' ? '定时排风' : d.mode === 'timed_off' ? '临时暂停' : '持续关闭';
    $('#delayPill').textContent = d.mode.startsWith('timed_')
      ? '恢复自动 ' + pretty(d.remaining)
      : d.mode === 'lock_off' ? '需要手动解除' : `延时排风 ${d.exhaustDelay / 1000} 秒`;
    $('#autoLabel').textContent = isAuto ? '✓　已启用自动感应' : '⟳　切换到自动感应';
    $('#autoHint').textContent = isAuto
      ? (d.suppressed ? '等待本轮 PIR 信号复位，之后继续自动排风' : '检测到活动时开启；离开后延时关闭')
      : '立即停止风机，结束临时控制并恢复 PIR 自动';
    autoButton.classList.toggle('selected', isAuto);
    autoButton.setAttribute('aria-pressed', String(isAuto));

    document.querySelectorAll('[data-timed]').forEach(button => {
      const selected = d.mode === `timed_${button.dataset.timed}` && Number(button.dataset.seconds) * 1000 === d.duration;
      button.classList.toggle('selected', selected);
      button.setAttribute('aria-pressed', String(selected));
    });
  } catch {
    $('#net').textContent = '连接中断';
    $('#net').style.color = 'var(--red)';
  }
}
document.querySelector('[data-auto]').addEventListener('click',setAuto);refresh();setInterval(refresh,1000);
</script></body></html>
)HTML";

uint8_t relayOnLevel() { return RELAY_ACTIVE_LOW ? LOW : HIGH; }
uint8_t relayOffLevel() { return RELAY_ACTIVE_LOW ? HIGH : LOW; }

// 仅在风机状态变化时驱动继电器，避免重复写入和刷屏。
void setFan(bool turnOn) {
  if (fanOn == turnOn) return;
  fanOn = turnOn;
  if (fanOn) fanStartedMs = millis();
  digitalWrite(RELAY_PIN, fanOn ? relayOnLevel() : relayOffLevel());
  Serial.printf("[%lu ms] Fan: %s\n", millis(), fanOn ? "ON" : "OFF");
}

const char* modeName() {
  switch (controlMode) {
    case ControlMode::TIMED_ON: return "timed_on";
    case ControlMode::TIMED_OFF: return "timed_off";
    case ControlMode::LOCKED_OFF: return "lock_off";
    default: return "auto";
  }
}

void handleRoot() { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }

// 手机页面每秒读取一次这份状态；只返回设备真实采集到的状态，不伪造传感器数据。
void handleStatus() {
  const unsigned long now = millis();
  const unsigned long runMs = fanOn ? now - fanStartedMs : 0;
  unsigned long remaining = 0;
  if (controlMode == ControlMode::TIMED_ON || controlMode == ControlMode::TIMED_OFF) {
    const unsigned long elapsed = now - overrideStartedMs;
    remaining = elapsed >= overrideDurationMs ? 0 : overrideDurationMs - elapsed;
  } else if (controlMode == ControlMode::AUTO && fanOn && !presenceDetected) {
    const unsigned long elapsed = now - lastPresenceMs;
    const unsigned long exhaustRemaining = elapsed >= EXHAUST_DELAY_MS ? 0 : EXHAUST_DELAY_MS - elapsed;
    const unsigned long runElapsed = now - fanStartedMs;
    const unsigned long minimumRunRemaining = runElapsed >= FAN_MIN_RUN_MS ? 0 : FAN_MIN_RUN_MS - runElapsed;
    // 两个关机条件都满足才会停机，因此倒计时显示还未满足条件中较长的一项。
    remaining = exhaustRemaining > minimumRunRemaining ? exhaustRemaining : minimumRunRemaining;
  }
  char payload[280];
  snprintf(payload, sizeof(payload),
           "{\"fan\":%s,\"pir\":%s,\"suppressed\":%s,\"mode\":\"%s\",\"runMs\":%lu,\"remaining\":%lu,\"duration\":%lu,\"exhaustDelay\":%lu}",
           fanOn ? "true" : "false", presenceDetected ? "true" : "false",
           suppressPresenceUntilLow ? "true" : "false", modeName(),
           runMs, remaining, overrideDurationMs, EXHAUST_DELAY_MS);
  server.send(200, "application/json; charset=utf-8", payload);
}

void returnToAuto(bool stopImmediately = false) {
  controlMode = ControlMode::AUTO;
  overrideDurationMs = 0;
  // 直接点“自动感应”会立即停止风机，并忽略当前这轮已触发的 PIR 高电平；
  // 等 PIR 回到 LOW 后重新开始感应，避免下一轮 loop 又立刻把风机打开。
  suppressPresenceUntilLow = stopImmediately && presenceDetected;
  if (stopImmediately) setFan(false);
  if (!presenceDetected) lastPresenceMs = millis();
  Serial.println("Control mode: auto");
}

void handleTimedOverride() {
  if (!server.hasArg("mode") || !server.hasArg("seconds")) {
    server.send(400, "application/json", "{\"error\":\"mode and seconds required\"}"); return;
  }
  const String mode = server.arg("mode");
  const long seconds = server.arg("seconds").toInt();
  if ((mode != "on" && mode != "off") ||
      (seconds != 30 && seconds != 60 && seconds != 300 && seconds != 600)) {
    server.send(400, "application/json", "{\"error\":\"unsupported timed override\"}"); return;
  }
  controlMode = mode == "on" ? ControlMode::TIMED_ON : ControlMode::TIMED_OFF;
  overrideStartedMs = millis();
  overrideDurationMs = static_cast<unsigned long>(seconds) * 1000UL;
  suppressPresenceUntilLow = false;
  Serial.printf("Timed override: %s for %ld seconds\n", mode.c_str(), seconds);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetAuto() {
  returnToAuto(true);
  server.send(200, "application/json", "{\"ok\":true}");
}

// 持续关闭采用服务端两阶段确认；准备状态 15 秒后自动失效。
void handleLockOff() {
  const String stage = server.arg("stage");
  const unsigned long now = millis();
  if (stage == "prepare") {
    lockOffArmed = true;
    lockOffArmedMs = now;
    server.send(200, "application/json", "{\"armed\":true}");
  } else if (stage == "confirm") {
    if (!lockOffArmed || now - lockOffArmedMs > 15000UL) {
      lockOffArmed = false;
      server.send(409, "application/json", "{\"error\":\"confirmation expired\"}");
      return;
    }
    lockOffArmed = false;
    controlMode = ControlMode::LOCKED_OFF;
    overrideDurationMs = 0;
    Serial.println("Control mode: lock_off");
    server.send(200, "application/json", "{\"ok\":true}");
  } else if (stage == "cancel") {
    lockOffArmed = false;
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(400, "application/json", "{\"error\":\"invalid stage\"}");
  }
}

void handleNotFound() { server.send(404, "text/plain; charset=utf-8", "Not found"); }

void setup() {
  Serial.begin(115200);
  digitalWrite(RELAY_PIN, relayOffLevel());
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, relayOffLevel());
  pinMode(PIR_PIN, INPUT);
  statusLedBegin();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  const bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/timed", HTTP_POST, handleTimedOverride);
  server.on("/api/auto", HTTP_POST, handleSetAuto);
  server.on("/api/lockoff", HTTP_POST, handleLockOff);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("\nLitterBoxVentilation ready");
  Serial.printf("AP: %s / %s\n", AP_SSID, AP_PASSWORD);
  Serial.printf("Dashboard: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("AP status: %s; PIR GPIO4; Relay GPIO5; WS2812 GPIO8\n", apStarted ? "started" : "failed");
  Serial.printf("Timing: %s; exhaust %lu ms; min run %lu ms\n",
                DEMO_MODE ? "DEMO" : "PRODUCTION", EXHAUST_DELAY_MS, FAN_MIN_RUN_MS);
}

void loop() {
  server.handleClient();
  const unsigned long now = millis();

  if (now - lastPirPollMs >= PIR_POLL_INTERVAL_MS) {
    lastPirPollMs = now;
    presenceDetected = digitalRead(PIR_PIN) == HIGH;
    if (presenceDetected != previousPresenceDetected) {
      Serial.printf("[%lu ms] PIR: %s\n", now, presenceDetected ? "HIGH" : "LOW");
      previousPresenceDetected = presenceDetected;
    }
    if (suppressPresenceUntilLow && !presenceDetected) {
      suppressPresenceUntilLow = false;
      lastPresenceMs = now;
    }
    if (presenceDetected && !suppressPresenceUntilLow) lastPresenceMs = now;
  }

  // 定时覆盖到点后自动回到 AUTO；millis 差值写法可正确处理计数回绕。
  if ((controlMode == ControlMode::TIMED_ON || controlMode == ControlMode::TIMED_OFF) &&
      now - overrideStartedMs >= overrideDurationMs) {
    returnToAuto();
  }

  // 定时模式和锁定模式优先于 PIR；到时自动切回感应，锁定关闭只能手动解除或重启。
  if (controlMode == ControlMode::TIMED_ON) {
    setFan(true);
  } else if (controlMode == ControlMode::TIMED_OFF || controlMode == ControlMode::LOCKED_OFF) {
    setFan(false);
  } else if (presenceDetected && !suppressPresenceUntilLow) {
    setFan(true);
  } else if (fanOn && now - lastPresenceMs >= EXHAUST_DELAY_MS &&
             now - fanStartedMs >= FAN_MIN_RUN_MS) {
    setFan(false);
  }

  if (now - lastLedUpdateMs >= 20UL) {
    lastLedUpdateMs = now;
    statusLedUpdate(fanOn, now);
  }
  delay(1);  // 让出少量 CPU 时间，保持网页请求响应及时。
}
