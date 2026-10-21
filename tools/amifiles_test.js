/* Test main/web/amifiles.js: archives made here (DMS stored and RLE
 * tracks, a gzipped ADF), then each DMS archive (and .adz) in a folder tree
 * unpacked and compared with the ADF beside it.
 *   node tools/amifiles_test.js [folder] */
"use strict";
const fs = require("fs"), path = require("path");
const ami = require("../main/web/amifiles.js");

const zlib = require("zlib");

function crc16(b) {
  let c = 0;
  for (const x of b) { c ^= x; for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xa001 : c >>> 1; }
  return c;
}

/* RLE as DMS packs it: 0x90 escapes, runs of 4 and more */
function rlePack(b) {
  const out = [];
  for (let i = 0; i < b.length;) {
    let n = 1;
    while (i + n < b.length && b[i + n] === b[i] && n < 65535) n++;
    if (n >= 4) {
      out.push(0x90, n < 255 ? n : 0xff, b[i]);
      if (n >= 255) out.push(n >> 8, n & 255);
      i += n;
    } else {
      for (let k = 0; k < n; k++) b[i] === 0x90 ? out.push(0x90, 0) : out.push(b[i]);
      i += n;
    }
  }
  return Uint8Array.from(out);
}

/* A DMS archive of an ADF, its tracks stored (mode 0) or RLE'd (mode 1) */
function makeDms(adf, mode) {
  const head = new Uint8Array(56);
  head.set([0x44, 0x4d, 0x53, 0x21]);
  head[19] = 79;
  const c = crc16(head.subarray(4, 54));
  head[54] = c >> 8; head[55] = c & 255;
  const parts = [head];
  for (let t = 0; t < 80; t++) {
    const raw = adf.subarray(t * 11264, (t + 1) * 11264);
    const data = mode ? rlePack(Uint8Array.from(raw)) : raw;
    const h = new Uint8Array(20);
    h[0] = 0x54; h[1] = 0x52; h[3] = t;
    h[6] = data.length >> 8; h[7] = data.length & 255;
    h[8] = data.length >> 8; h[9] = data.length & 255;
    h[10] = 11264 >> 8; h[11] = 11264 & 255;
    h[13] = mode;
    let sum = 0;
    for (const x of raw) sum += x;
    h[14] = (sum >> 8) & 255; h[15] = sum & 255;
    const dc = crc16(data);
    h[16] = dc >> 8; h[17] = dc & 255;
    const hc = crc16(h.subarray(0, 18));
    h[18] = hc >> 8; h[19] = hc & 255;
    parts.push(h, data);
  }
  return Buffer.concat(parts.map(p => Buffer.from(p)));
}

async function made() {
  /* a disk with runs, 0x90 bytes and noise */
  const adf = new Uint8Array(901120);
  let x = 1;
  for (let i = 0; i < adf.length; i++) {
    x = (x * 1103515245 + 12345) >>> 0;
    adf[i] = i % 4096 < 1024 ? 0 : i % 7 === 0 ? 0x90 : (x >>> 16) & 255;
  }
  let bad = 0;
  for (const [name, data] of [["stored.dms", makeDms(adf, 0)], ["rle.dms", makeDms(adf, 1)], ["disk.adz", zlib.gzipSync(adf)]]) {
    const [out] = await ami.convert(name, new Uint8Array(data));
    const same = Buffer.compare(Buffer.from(out.data), Buffer.from(adf)) === 0;
    console.log((same ? "ok  " : "BAD ") + name + " (made here, " + data.length + " bytes)");
    bad += same ? 0 : 1;
  }
  return bad;
}

async function main() {
  let made_bad = await made();
  if (!process.argv[2]) process.exit(made_bad ? 1 : 0);
  const files = [];
  const walk = d => fs.readdirSync(d, { withFileTypes: true }).forEach(e =>
    e.isDirectory() ? walk(path.join(d, e.name)) : /\.(dms|adz)$/i.test(e.name) && files.push(path.join(d, e.name)));
  walk(process.argv[2]);
  let ok = 0, bad = 0;
  for (const f of files) {
    const base = f.replace(/\.[^.]*$/, "");
    const adf = [base + ".ADF", base + ".adf"].find(p => fs.existsSync(p));
    try {
      const t0 = Date.now();
      const [out] = await ami.convert(path.basename(f), new Uint8Array(fs.readFileSync(f)));
      const ms = Date.now() - t0;
      if (!adf) { console.log("?   " + f + ": " + out.data.length + " bytes, no ADF to compare"); continue; }
      const want = fs.readFileSync(adf);
      const same = Buffer.compare(Buffer.from(out.data), want) === 0;
      console.log((same ? "ok  " : "BAD ") + path.relative(process.argv[2], f) + " (" + ms + " ms)");
      same ? ok++ : bad++;
    } catch (e) {
      console.log("ERR " + path.relative(process.argv[2], f) + ": " + e.message);
      bad++;
    }
  }
  console.log(ok + " identical, " + bad + " not");
  process.exit(bad || made_bad ? 1 : 0);
}
main();
