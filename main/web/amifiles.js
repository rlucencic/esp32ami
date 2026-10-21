/* Amiga files on the web page: DMS (DiskMasher) archives and gzipped disk
 * images (.adz, .hdz) are unpacked in the browser into ADFs and HDFs,
 * which are uploaded like any other image.
 *
 * The board serves this file as /machine.js; the page loads it when
 * /api/info says "script": true, and it registers itself with
 * registerConverter(). It also runs under node (module.exports) for tests:
 * tools/amifiles_test.js.
 *
 * DMS: a 56-byte header, then the tracks, each a 20-byte header ("TR",
 * number, packed and unpacked lengths, flags, mode, checksums) and its
 * data. The modes follow xDMS (Andre Rodrigues de la Rocha, public
 * domain): 0 stored, 1 RLE, 2 quick (LZ, 256-byte window), 3 medium (LZ
 * with LZHUF's position codes), 4 deep (LZHUF's adaptive Huffman), 5 and 6
 * heavy (LHA's static Huffman, 4 and 8 KB windows); the windows and the
 * deep tree go on from track to track unless a track's flag says to start
 * again. Each track's checksum is checked. Encrypted archives aren't.
 */
(function () {
"use strict";

class AmigaFileError extends Error {}
const fail = msg => { throw new AmigaFileError(msg); };

const u16 = (b, p) => (b[p] << 8) | b[p + 1];

/* CRC-16 (reflected, 0xA001): DMS's headers and packed data */
const CRC16 = new Uint16Array(256);
for (let i = 0; i < 256; i++) {
  let c = i;
  for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xa001 : c >>> 1;
  CRC16[i] = c;
}
function crc16(b, p, n) {
  let c = 0;
  for (let i = p; i < p + n; i++) c = CRC16[(c ^ b[i]) & 255] ^ (c >>> 8);
  return c;
}

/* ---- DMS ---- */

/* MSB first; at least 16 bits ahead (xDMS's GETBITS/DROPBITS) */
function bits(b, p) {
  let buf = 0, count = 0;
  const fill = () => { while (count < 16) { buf = ((buf << 8) | (p < b.length ? b[p] : 0)) >>> 0; p++; count += 8; } };
  fill();
  return {
    get: n => (buf >>> (count - n)) & ((1 << n) - 1),
    drop: n => { count -= n; buf &= (2 ** count) - 1; fill(); },
  };
}

/* LZHUF's position codes (Haruyasu Yoshizaki): the upper 6 bits of a
 * position from its first byte, and how many more bits it takes */
const D_CODE = new Uint8Array(256), D_LEN = new Uint8Array(256);
{
  let i = 0, code = 0;
  for (const [codes, each, len] of [[1, 32, 3], [3, 16, 4], [8, 8, 5], [12, 4, 6], [24, 2, 7], [16, 1, 8]]) {
    for (let c = 0; c < codes; c++, code++) for (let k = 0; k < each; k++, i++) { D_CODE[i] = code; D_LEN[i] = len; }
  }
}

/* A match may run past the track's end, as in xDMS: its window and
 * position go on, the bytes past the end are dropped */
const MARGIN = 512;

function rle(src, n) {
  const out = new Uint8Array(n);
  let o = 0, p = 0;
  while (o < n) {
    const a = src[p++];
    if (a !== 0x90) { out[o++] = a; continue; }
    const b = src[p++];
    if (b === 0) { out[o++] = a; continue; }
    const v = src[p++];
    let count = b;
    if (b === 0xff) { count = (src[p] << 8) | src[p + 1]; p += 2; }
    if (o + count > n) fail("bad RLE data");
    out.fill(v, o, o + count);
    o += count;
  }
  return out;
}

/* A canonical Huffman code from its lengths (as LHA's make_table builds
 * it: shorter codes first, then by symbol); one symbol and no lengths:
 * that symbol costs no bits */
function huffman(lens, single) {
  if (single !== undefined) return { decode: () => single };
  const count = new Uint16Array(33), first = new Uint32Array(33), offset = new Uint16Array(33);
  for (const l of lens) count[l]++;
  count[0] = 0;
  const sorted = [];
  for (let l = 1; l <= 32; l++) for (let s = 0; s < lens.length; s++) if (lens[s] === l) sorted.push(s);
  let code = 0, at = 0;
  for (let l = 1; l <= 32; l++) {
    code = (code + count[l - 1]) << 1;
    first[l] = code;
    offset[l] = at;
    at += count[l];
  }
  return {
    decode(r) {
      /* up to 24 bits at a time from the reader's 16-bit window */
      let code = 0;
      for (let l = 1; l <= 24; l++) {
        code = (code << 1) | r.get(1);
        r.drop(1);
        if (code - first[l] < count[l]) return sorted[offset[l] + code - first[l]];
      }
      fail("bad Huffman code");
    },
  };
}

class Dms {
  constructor() { this.text = new Uint8Array(0x4000); this.init(); }

  init() {
    this.quick = 251;
    this.medium = 0x3fbe;
    this.heavy = 0;
    this.deep = 0x3fc4;
    this.deepTree = null;
    this.text.fill(0);
  }

  quickUnpack(src, n) {
    const out = new Uint8Array(n + MARGIN), t = this.text, r = bits(src, 0);
    let o = 0, loc = this.quick;
    while (o < n) {
      if (r.get(1)) {
        r.drop(1);
        out[o++] = t[loc++ & 0xff] = r.get(8);
        r.drop(8);
      } else {
        r.drop(1);
        let j = r.get(2) + 2;
        r.drop(2);
        let i = loc - r.get(8) - 1;
        r.drop(8);
        while (j--) out[o++] = t[loc++ & 0xff] = t[i++ & 0xff];
      }
    }
    this.quick = (loc + 5) & 0xff;
    return out.subarray(0, n);
  }

  mediumUnpack(src, n) {
    const out = new Uint8Array(n + MARGIN), t = this.text, r = bits(src, 0);
    let o = 0, loc = this.medium;
    while (o < n) {
      if (r.get(1)) {
        r.drop(1);
        out[o++] = t[loc++ & 0x3fff] = r.get(8);
        r.drop(8);
      } else {
        r.drop(1);
        let c = r.get(8);
        r.drop(8);
        let j = D_CODE[c] + 3, u = D_LEN[c];
        c = ((c << u) | r.get(u)) & 0xff;
        r.drop(u);
        u = D_LEN[c];
        c = (D_CODE[c] << 8) | (((c << u) | r.get(u)) & 0xff);
        r.drop(u);
        let i = loc - c - 1;
        while (j--) out[o++] = t[loc++ & 0x3fff] = t[i++ & 0x3fff];
      }
    }
    this.medium = (loc + 66) & 0x3fff;
    return out.subarray(0, n);
  }

  /* LZHUF's adaptive Huffman tree: 314 codes (256 bytes, 58 lengths) */
  deepUnpack(src, n) {
    const N_CHAR = 256 - 2 + 60, T = N_CHAR * 2 - 1, R = T - 1, MAX_FREQ = 0x8000;
    if (!this.deepTree) {
      const freq = new Uint16Array(T + 1), son = new Uint16Array(T), prnt = new Uint16Array(T + N_CHAR);
      for (let i = 0; i < N_CHAR; i++) { freq[i] = 1; son[i] = i + T; prnt[i + T] = i; }
      for (let i = 0, j = N_CHAR; j <= R; i += 2, j++) {
        freq[j] = freq[i] + freq[i + 1];
        son[j] = i;
        prnt[i] = prnt[i + 1] = j;
      }
      freq[T] = 0xffff;
      prnt[R] = 0;
      this.deepTree = { freq, son, prnt };
    }
    const { freq, son, prnt } = this.deepTree;
    const reconst = () => {
      let j = 0;
      for (let i = 0; i < T; i++) {
        if (son[i] >= T) { freq[j] = (freq[i] + 1) >> 1; son[j] = son[i]; j++; }
      }
      for (let i = 0, j2 = N_CHAR; j2 < T; i += 2, j2++) {
        const f = freq[j2] = freq[i] + freq[i + 1];
        let k = j2 - 1;
        while (f < freq[k]) k--;
        k++;
        freq.copyWithin(k + 1, k, j2);
        freq[k] = f;
        son.copyWithin(k + 1, k, j2);
        son[k] = i;
      }
      for (let i = 0; i < T; i++) {
        const k = son[i];
        if (k >= T) prnt[k] = i;
        else prnt[k] = prnt[k + 1] = i;
      }
    };
    const update = code => {
      if (freq[R] === MAX_FREQ) reconst();
      let c = prnt[code + T];
      do {
        const k = ++freq[c];
        let l = c + 1;
        if (k > freq[l]) {
          while (k > freq[++l]);
          l--;
          freq[c] = freq[l];
          freq[l] = k;
          const i = son[c];
          prnt[i] = l;
          if (i < T) prnt[i + 1] = l;
          const j = son[l];
          son[l] = i;
          prnt[j] = c;
          if (j < T) prnt[j + 1] = c;
          son[c] = j;
          c = l;
        }
        c = prnt[c];
      } while (c !== 0);
    };
    const out = new Uint8Array(n + MARGIN), t = this.text, r = bits(src, 0);
    let o = 0, loc = this.deep;
    while (o < n) {
      let c = son[R];
      while (c < T) { c = son[c + r.get(1)]; r.drop(1); }
      c -= T;
      update(c);
      if (c < 256) {
        out[o++] = t[loc++ & 0x3fff] = c;
      } else {
        let j = c - 255 + 2;
        let i = r.get(8);
        r.drop(8);
        const hi = D_CODE[i] << 8, len = D_LEN[i];
        i = ((i << len) | r.get(len)) & 0xff;
        r.drop(len);
        let from = loc - (hi | i) - 1;
        while (j--) out[o++] = t[loc++ & 0x3fff] = t[from++ & 0x3fff];
      }
    }
    this.deep = (loc + 60) & 0x3fff;
    return out.subarray(0, n);
  }

  /* LHA's -lh5- style: codes for bytes and lengths, codes for positions */
  heavyUnpack(src, n, flags) {
    const np = flags & 8 ? 15 : 14, mask = flags & 8 ? 0x1fff : 0x0fff;
    const r = bits(src, 0);
    if (flags & 2) {
      let k = r.get(9);
      r.drop(9);
      if (k > 0) {
        const lens = new Uint8Array(510);
        for (let i = 0; i < k; i++) { lens[i] = r.get(5); r.drop(5); }
        this.cTree = huffman(lens);
      } else {
        const s = r.get(9);
        r.drop(9);
        this.cTree = huffman(null, s);
      }
      k = r.get(5);
      r.drop(5);
      if (k > 0) {
        const lens = new Uint8Array(np);
        for (let i = 0; i < k; i++) { lens[i] = r.get(4); r.drop(4); }
        this.pTree = huffman(lens);
      } else {
        const s = r.get(5);
        r.drop(5);
        this.pTree = huffman(null, s);
      }
    }
    if (!this.cTree || !this.pTree) fail("heavy track without its codes");
    const out = new Uint8Array(n + MARGIN), t = this.text;
    let o = 0, loc = this.heavy;
    while (o < n) {
      const c = this.cTree.decode(r);
      if (c < 256) {
        out[o++] = t[loc++ & mask] = c;
      } else {
        let j = c - 253;
        let p = this.pTree.decode(r);
        if (p !== np - 1) {
          if (p > 0) { const extra = p - 1; p = r.get(extra) | (1 << extra); r.drop(extra); }
          this.lastLen = p;
        }
        let from = loc - (this.lastLen || 0) - 1;
        while (j--) out[o++] = t[loc++ & mask] = t[from++ & mask];
      }
    }
    this.heavy = loc & 0xffff;
    return out.subarray(0, n);
  }

  track(mode, flags, data, packed2, size) {
    switch (mode) {
    case 0: return data.slice(0, size);
    case 1: return rle(data, size);
    case 2: return rle(this.quickUnpack(data, packed2), size);
    case 3: return rle(this.mediumUnpack(data, packed2), size);
    case 4: return rle(this.deepUnpack(data, packed2), size);
    case 5: case 6: {
      const out = this.heavyUnpack(data, packed2, mode === 5 ? flags & 7 : flags | 8);
      return flags & 4 ? rle(out, size) : out;
    }
    default: fail("compression mode " + mode + " unknown");
    }
  }
}

/* A DMS archive's ADF */
function unDms(name, b) {
  if (b.length < 56 || String.fromCharCode(...b.subarray(0, 4)) !== "DMS!") fail(name + ": not a DMS archive");
  if (crc16(b, 4, 50) !== u16(b, 54)) fail(name + ": damaged header");
  if (u16(b, 10) & 2) fail(name + ": encrypted (needs its password)");
  const dms = new Dms();
  const tracks = [];
  let p = 56, trackSize = 0;
  while (p + 20 <= b.length) {
    if (b[p] !== 0x54 || b[p + 1] !== 0x52) break;          /* "TR" */
    if (crc16(b, p, 18) !== u16(b, p + 18)) fail(name + ": damaged track header at " + p);
    const number = u16(b, p + 2), packed = u16(b, p + 6), packed2 = u16(b, p + 8), size = u16(b, p + 10);
    const flags = b[p + 12], mode = b[p + 13], sum = u16(b, p + 14), dataCrc = u16(b, p + 16);
    const data = b.subarray(p + 20, p + 20 + packed);
    if (data.length < packed) fail(name + ": cut short in track " + number);
    if (crc16(b, p + 20, packed) !== dataCrc) fail(name + ": damaged data in track " + number);
    const out = dms.track(mode, flags, data, packed2, size);
    let s = 0;
    for (let i = 0; i < size; i++) s += out[i];
    if ((s & 0xffff) !== sum) fail(name + ": track " + number + " didn't unpack right (mode " + mode + ")");
    /* the disk's tracks; banners and FILEID.DIZ (other numbers, sizes) left out */
    if (number < 80 && (size === 11264 || size === 22528)) {
      trackSize = trackSize || size;
      if (size === trackSize) tracks[number] = out;
    }
    if (!(flags & 1)) dms.init();
    p += 20 + packed;
  }
  if (!trackSize) fail(name + ": no disk tracks in it");
  const adf = new Uint8Array(80 * trackSize);
  tracks.forEach((t, i) => t && adf.set(t, i * trackSize));
  return adf;
}

/* ---- gzip (.adz, .hdz) ---- */

async function gunzip(name, b) {
  if (b[0] !== 0x1f || b[1] !== 0x8b) fail(name + ": not gzipped");
  if (typeof DecompressionStream === "undefined") {
    return new Uint8Array(require("zlib").gunzipSync(b));
  }
  const stream = new Blob([b]).stream().pipeThrough(new DecompressionStream("gzip"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

/* name, bytes -> [{name, data}] for the board */
async function convert(name, bytes, opts = {}) {
  const log = opts.log || (() => {});
  const lower = name.toLowerCase(), base = name.replace(/\.[^.]*$/, "");
  let data, out;
  if (lower.endsWith(".dms")) {
    data = unDms(name, bytes);
    out = base + ".adf";
  } else {
    data = await gunzip(name, bytes);
    out = base + (lower.endsWith(".hdz") ? ".hdf" : ".adf");
  }
  log(name + ": unpacked to " + out + " (" + (data.length >> 10) + " KB)");
  return [{ name: out, data }];
}

const converter = {
  description: "DMS archives and gzipped images (.DMS, .ADZ, .HDZ)",
  extensions: [".dms", ".adz", ".hdz"],
  convert,
};

if (typeof window !== "undefined" && window.registerConverter) window.registerConverter(converter);
if (typeof module !== "undefined") module.exports = { ...converter, unDms, AmigaFileError };
})();
