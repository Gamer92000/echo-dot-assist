// tar for the settings page's "Models on file" (app.js): the file any computer opens.  Written as ustar (a path longer
// than 100 bytes split into prefix and name at a slash, as tar(1) and Python's tarfile do); read back also with GNU's
// long names (././@LongLink) and pax paths, for a file someone unpacked and packed again.  Nothing but regular files
// is taken.  tests/unit/web_tar_test.py checks both ways against Python's tarfile.
'use strict';

const hmtar = (() => {
  const enc = new TextEncoder(), dec = new TextDecoder();

  function put(b, off, len, s) {
    const e = enc.encode(s);
    if (e.length > len) throw new Error(`tar: "${s}" does not fit`);
    b.set(e, off);
  }
  const str = (b, off, len) => { const s = b.subarray(off, off + len), z = s.indexOf(0); return dec.decode(z < 0 ? s : s.subarray(0, z)); };
  // octal digits only: parseInt alone takes "-1750" (a size that walks backwards: the reader would never end) and stops
  // quietly at junk; GNU's base-256 numbers (past 8 GB) come out NaN and are refused with it
  const num = (b, off, len) => { const s = str(b, off, len).trim(); return /^[0-7]*$/.test(s) ? parseInt(s || '0', 8) : NaN; };
  const META_MAX = 65536;                                      // a long name or pax record; more is no tar of ours
  const clean = (p) => p.replace(/^(\.?\/)+/, '');             // "./x" of `tar -cf f.tar .`, "/x"
  const sum = (b) => { let n = 0; for (let i = 0; i < 512; i++) n += i >= 148 && i < 156 ? 32 : b[i]; return n; };   // its own field as spaces

  // ustar's name field holds 100 bytes; longer paths go into the prefix (155) up to a slash
  function split(path) {
    if (enc.encode(path).length <= 100) return ['', path];
    for (let i = path.indexOf('/'); i > 0; i = path.indexOf('/', i + 1)) {
      const p = path.slice(0, i), n = path.slice(i + 1);
      if (n && enc.encode(p).length <= 155 && enc.encode(n).length <= 100) return [p, n];
    }
    throw new Error(`tar: ${path} is too long`);
  }

  function header(path, size, mtime) {
    const h = new Uint8Array(512), [prefix, name] = split(path);
    put(h, 0, 100, name); put(h, 100, 8, '0000644'); put(h, 108, 8, '0000000'); put(h, 116, 8, '0000000');
    put(h, 124, 12, size.toString(8).padStart(11, '0')); put(h, 136, 12, mtime.toString(8).padStart(11, '0'));
    h[156] = 48;                                               // '0': a regular file
    put(h, 257, 6, 'ustar'); put(h, 263, 2, '00'); put(h, 345, 155, prefix);
    put(h, 148, 8, sum(h).toString(8).padStart(6, '0') + '\0 ');
    return h;
  }

  // files: [{ path, parts: [Uint8Array or Blob, ...] }] -> the pieces of the tar, for new Blob(...): nothing copied twice
  function write(files, mtime = Math.floor(Date.now() / 1000)) {
    const out = [];
    for (const f of files) {
      const size = f.parts.reduce((n, p) => n + (p instanceof Uint8Array ? p.length : p.size), 0);
      out.push(header(f.path, size, mtime), ...f.parts, new Uint8Array((512 - (size % 512)) % 512));
    }
    out.push(new Uint8Array(1024));                            // two empty blocks end it
    return out;
  }

  // the regular files of a tar of TOTAL bytes read through read(off, len) -> Promise<Uint8Array> (a File's slices, so
  // a big one is never all in memory): [{ path, size, off }], off where the content starts
  async function list(read, total) {
    const out = [];
    let off = 0, long = null;
    while (off + 512 <= total) {
      const h = await read(off, 512);
      if (h.every((x) => x === 0)) break;
      const size = num(h, 124, 12), type = h[156] ? String.fromCharCode(h[156]) : '0', data = off + 512;
      if (num(h, 148, 8) !== sum(h) || !Number.isSafeInteger(size)) throw new Error('not a tar file, or a damaged one');
      if (data + size > total) throw new Error('the file is cut short');
      if ((type === 'L' || type === 'x') && size > META_MAX) throw new Error('not a tar file, or a damaged one');
      const ustar = str(h, 257, 6) === 'ustar', prefix = ustar ? str(h, 345, 155) : '';  // GNU's own keeps other things there
      if (type === 'L') long = str(await read(data, size), 0, size);                     // the next one's name
      else if (type === 'x') {                                                           // pax: "<len> path=<path>\n" records
        const m = /(?:^|\n)\d+ path=([^\n]*)\n/.exec(dec.decode(await read(data, size)));
        if (m) long = m[1];
      } else if (type !== 'g') {
        if (type === '0' || type === '7') out.push({ path: clean(long || (prefix ? prefix + '/' : '') + str(h, 0, 100)), size, off: data });
        long = null;
      }
      off = data + Math.ceil(size / 512) * 512;
    }
    return out;
  }

  return { write, list, header };
})();

if (typeof module !== 'undefined') module.exports = hmtar;
