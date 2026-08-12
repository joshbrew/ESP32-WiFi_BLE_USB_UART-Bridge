#!/usr/bin/env node

/*
  Build the ESP32-hosted web console without third-party packages.

  Inputs:
    web/index.html
    web/app.css
    web/app.js

  Outputs:
    src/web/WebAssets.h          Deterministic gzip bytes embedded by firmware
    web/standalone_console.html  Self-contained laptop version

  Usage:
    node web/build_web_assets.mjs
    node web/build_web_assets.mjs --check
*/

import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";

const webDirectory = path.dirname(fileURLToPath(import.meta.url));
const projectDirectory = path.dirname(webDirectory);
const embeddedPath = path.join(projectDirectory, "src", "web", "WebAssets.h");
const standalonePath = path.join(webDirectory, "standalone_console.html");
const args = new Set(process.argv.slice(2));

if (args.has("--help") || args.has("-h")) {
  console.log(`Usage: node web/build_web_assets.mjs [--check]

With no option, rebuild WebAssets.h and standalone_console.html.
With --check, make no changes and fail if either generated file is stale.`);
  process.exit(0);
}

for (const argument of args) {
  if (argument !== "--check") {
    throw new Error(`Unknown option: ${argument}. Use --help for usage.`);
  }
}

function readSource(filename) {
  return fs
    .readFileSync(path.join(webDirectory, filename), "utf8")
    .replace(/^\uFEFF/, "")
    .replace(/\r\n/g, "\n");
}

const indexHtml = readSource("index.html");
const appCss = readSource("app.css").trimEnd();
const appJs = readSource("app.js").trimEnd();
const stylesheetMarker = '  <link rel="stylesheet" href="/app.css">';
const scriptMarker = '  <script src="/app.js"></script>';

if (!indexHtml.includes(stylesheetMarker) || !indexHtml.includes(scriptMarker)) {
  throw new Error(
    "index.html must contain the normal /app.css and /app.js tags so they can be inlined."
  );
}

const stylesheetBlock = `  <style>\n${appCss}\n</style>`;
const scriptBlock = `  <script>\n${appJs}\n</script>`;
const portalHtml = indexHtml
  .replace(stylesheetMarker, stylesheetBlock)
  .replace(scriptMarker, scriptBlock);

if (portalHtml.includes('/app.css') || portalHtml.includes('/app.js')) {
  throw new Error("The generated portal still contains external asset references.");
}

// Normalize gzip metadata so identical sources produce identical bytes on every
// machine. Bytes 4-7 are mtime and byte 9 is the gzip operating-system field.
const portalGzip = zlib.gzipSync(Buffer.from(portalHtml, "utf8"), {
  level: 9,
  mtime: 0
});
portalGzip[4] = 0;
portalGzip[5] = 0;
portalGzip[6] = 0;
portalGzip[7] = 0;
portalGzip[9] = 255;

if (zlib.gunzipSync(portalGzip).toString("utf8") !== portalHtml) {
  throw new Error("Internal verification failed: gzip did not reproduce the portal.");
}

const byteLines = [];
for (let offset = 0; offset < portalGzip.length; offset += 16) {
  const bytes = [...portalGzip.subarray(offset, offset + 16)]
    .map(value => `0x${value.toString(16).padStart(2, "0")}`)
    .join(", ");
  byteLines.push(`  ${bytes},`);
}

const embeddedHeader = `#ifndef DRONE_GEL_CONTROLLER_WEBASSETS_H
#define DRONE_GEL_CONTROLLER_WEBASSETS_H

// GENERATED FILE. Edit web/index.html, web/app.css, or web/app.js, then run:
//   node web/build_web_assets.mjs

#include <Arduino.h>

namespace WebAssets {

constexpr const char PORTAL_HTML_CONTENT_TYPE[] = "text/html; charset=utf-8";
constexpr const char PORTAL_HTML_CONTENT_ENCODING[] = "gzip";
static const uint8_t PORTAL_HTML_GZIP[] PROGMEM = {
${byteLines.join("\n")}
};
constexpr size_t PORTAL_HTML_GZIP_LENGTH = sizeof(PORTAL_HTML_GZIP);

}  // namespace WebAssets

#endif
`;

const standaloneCss = `
.standaloneToolbar {
  width: min(1180px, calc(100% - 24px));
  margin: 14px auto 0;
  display: grid;
  grid-template-columns: minmax(210px, 1fr) auto minmax(240px, 1fr);
  align-items: end;
  gap: 10px;
}
.standaloneToolbar label { display: grid; gap: 5px; color: var(--muted); font-size: .8rem; }
.standaloneToolbar input { width: 100%; padding: 9px 10px; }
.standaloneHint { color: var(--muted); font-size: .78rem; align-self: center; }
@media (max-width: 760px) { .standaloneToolbar { grid-template-columns: 1fr; } }
`;

const standaloneToolbar = `  <section class="standaloneToolbar panel">
    <label>Controller URL
      <input id="standaloneDeviceUrl" type="url" value="http://192.168.4.1" spellcheck="false">
    </label>
    <button id="standaloneUseDevice" class="primary">Use controller URL</button>
    <p id="standaloneDeviceStatus" class="standaloneHint">This standalone file is not served by the ESP32.</p>
  </section>`;

const standaloneSetup = `<script>
(() => {
  "use strict";
  const key = "drone-gel-device-url";
  const normalize = value => String(value || "").trim().replace(/\\/+$/, "") || "http://192.168.4.1";
  window.ESP32_API_BASE = normalize(localStorage.getItem(key));
  const input = document.getElementById("standaloneDeviceUrl");
  const button = document.getElementById("standaloneUseDevice");
  const status = document.getElementById("standaloneDeviceStatus");
  input.value = window.ESP32_API_BASE;
  status.textContent = "Standalone console targeting " + window.ESP32_API_BASE;
  button.addEventListener("click", () => {
    localStorage.setItem(key, normalize(input.value));
    window.location.reload();
  });
})();
</script>`;

let standaloneHtml = portalHtml.replace("\n</style>", `${standaloneCss}\n</style>`);
standaloneHtml = standaloneHtml.replace("<body>", `<body>\n${standaloneToolbar}`);
standaloneHtml = standaloneHtml.replace(scriptBlock, `${standaloneSetup}\n${scriptBlock}`);
standaloneHtml =
  `<!-- GENERATED STANDALONE FILE. Not embedded or served by firmware. -->\n${standaloneHtml}`;

function isCurrent(filename, expected) {
  return fs.existsSync(filename) &&
    fs.readFileSync(filename, "utf8").replace(/\r\n/g, "\n") === expected;
}

const outputs = [
  [embeddedPath, embeddedHeader],
  [standalonePath, standaloneHtml]
];

if (args.has("--check")) {
  const stale = outputs.filter(([filename, expected]) => !isCurrent(filename, expected));
  if (stale.length > 0) {
    for (const [filename] of stale) {
      console.error(`STALE: ${path.relative(projectDirectory, filename)}`);
    }
    console.error("Run: node web/build_web_assets.mjs");
    process.exit(1);
  }
  console.log(
    `Web assets are current: ${portalGzip.length} gzip bytes from ` +
      `${Buffer.byteLength(portalHtml)} HTML bytes.`
  );
  process.exit(0);
}

for (const [filename, contents] of outputs) {
  fs.writeFileSync(filename, contents, "utf8");
  console.log(`WROTE: ${path.relative(projectDirectory, filename)}`);
}
console.log(
  `Embedded portal: ${portalGzip.length} gzip bytes from ` +
    `${Buffer.byteLength(portalHtml)} HTML bytes.`
);
