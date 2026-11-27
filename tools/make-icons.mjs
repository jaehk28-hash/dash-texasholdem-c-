// web/logo.svg의 symbol(id="coupe")로 favicon.svg, favicon.ico, apple-touch-icon.png, og-image.png를 만든다.
// 로고를 고친 뒤에 한 번 실행하고 결과를 커밋한다.   사용법: node tools/make-icons.mjs
// 필요한 것: Node 22 이상, Google Chrome (다른 위치면 환경변수 CHROME에 실행 파일 경로).
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const web = resolve(fileURLToPath(import.meta.url), "../../web");
const symbol = readFileSync(join(web, "logo.svg"), "utf8").match(/<symbol id="coupe"[\s\S]*?<\/symbol>/)[0];
// viewBox와 선 두께만 바꿔서 같은 symbol을 쓰는 SVG
const art = (viewBox, width, height, style) =>
  `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${viewBox}" width="${width}" height="${height}" style="display:block;${style}">${symbol}<use href="#coupe"/></svg>`;
// 파비콘: 잔(폭 200)이 캔버스 폭의 약 65%. 작아서 선을 굵게 하고 기포는 뺀다.
const favicon = (size) => art("65 100 310 310", size, size, "--coupe-w:22px;--coupe-bubbles:none");

writeFileSync(join(web, "favicon.svg"), `<?xml version="1.0" encoding="UTF-8"?>\n<!-- tools/make-icons.mjs가 만든 파일. 직접 고치지 말고 logo.svg를 고친 뒤 다시 실행 -->\n${favicon(64)}\n`);

const work = mkdtempSync(join(tmpdir(), "coupe-icons-"));
const chrome = spawn(process.env.CHROME || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  ["--headless=new", "--remote-debugging-port=9444", `--user-data-dir=${work}/profile`, "--no-first-run", "about:blank"], { stdio: "ignore" });
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
let page;
for (let i = 0; i < 100 && !page; i++) {
  await sleep(200);
  try { page = (await (await fetch("http://127.0.0.1:9444/json")).json()).find((t) => t.type === "page"); } catch {}
}
if (!page) throw new Error("Chrome을 시작하지 못했습니다");
const socket = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((done) => (socket.onopen = done));
let id = 0;
const waiting = new Map();
socket.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id) waiting.get(m.id)(m.result); };
const send = (method, params = {}) => new Promise((done) => { waiting.set(++id, done); socket.send(JSON.stringify({ id, method, params })); });

async function render(html, width, height, transparent) {
  writeFileSync(join(work, "page.html"), `<!doctype html><meta charset="utf-8"><body style="margin:0;width:${width}px;height:${height}px;overflow:hidden">${html}</body>`);
  await send("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: 1, mobile: false });
  await send("Emulation.setDefaultBackgroundColorOverride", transparent ? { color: { r: 0, g: 0, b: 0, a: 0 } } : {});
  await send("Page.navigate", { url: pathToFileURL(join(work, "page.html")).href + "?" + Math.random() });
  await sleep(500);
  const shot = await send("Page.captureScreenshot", { clip: { x: 0, y: 0, width, height, scale: 1 } });
  return Buffer.from(shot.data, "base64");
}

const center = "display:flex;align-items:center;justify-content:center";
writeFileSync(join(web, "apple-touch-icon.png"), await render(`<div style="width:180px;height:180px;background:#0c100e;${center}">${favicon(170)}</div>`, 180, 180));

// ICO: PNG 세 장을 한 파일에
const images = [];
for (const size of [16, 32, 48]) images.push([size, await render(favicon(size), size, size, true)]);
const head = Buffer.alloc(6 + 16 * images.length);
head.writeUInt16LE(1, 2);
head.writeUInt16LE(images.length, 4);
let offset = head.length;
images.forEach(([size, png], i) => {
  const at = 6 + 16 * i;
  head.writeUInt8(size, at);
  head.writeUInt8(size, at + 1);
  head.writeUInt16LE(1, at + 4);
  head.writeUInt16LE(32, at + 6);
  head.writeUInt32LE(png.length, at + 8);
  head.writeUInt32LE(offset, at + 12);
  offset += png.length;
});
writeFileSync(join(web, "favicon.ico"), Buffer.concat([head, ...images.map(([, png]) => png)]));

writeFileSync(join(web, "og-image.png"), await render(`<div style="width:1200px;height:630px;display:flex;flex-direction:column;align-items:center;justify-content:center;background:radial-gradient(ellipse 60% 70% at 50% 32%, #1d3a2c, #0e1612 55%, #070a09)">
  ${art("108 116 224 278", 250, 310, "--coupe-w:6px")}
  <div style="margin-top:26px;font:800 66px Rockwell,'Bodoni 72',Georgia,serif;letter-spacing:5px;color:#e8cf85">COUPE HOLD'EM</div>
  <div style="margin-top:10px;font:700 30px Rockwell,Georgia,serif;letter-spacing:12px;padding-left:12px;color:#c9a24b">PRACTICE ROOM</div>
  <div style="margin-top:22px;font:700 36px 'Apple SD Gothic Neo',sans-serif;color:#f3ecd9">잔 한 모금처럼 가볍게 시작하는 카드게임</div></div>`, 1200, 630));

socket.close();
chrome.kill();
await sleep(500);
rmSync(work, { recursive: true, force: true });
console.log("만들었습니다: favicon.svg, favicon.ico, apple-touch-icon.png, og-image.png");
process.exit(0);
