/* Persistent game user data (settings, registry, saves) in the Origin Private File
 * System. The game worker mirrors every change under its in-memory /userdata into
 * ordered operations; this page-side store applies them asynchronously and restores
 * the tree before the next run. https://fs.spec.whatwg.org/#origin-private-file-system
 *
 * Operations: {op:'write', path, bytes} | {op:'delete', path} | {op:'mkdir', path}
 *           | {op:'rmdir', path} | {op:'rename', from, to}. Paths are relative to
 * /userdata, '/'-separated. */
const DIRECTORY = 'movies-userdata';

function parts(path) {
  const list = String(path).split('/').filter(Boolean);
  if (!list.length || list.some(p => p === '.' || p === '..')) throw new Error(`Invalid user-data path ${path}`);
  return list;
}

export class MoviesUserdataStore {
  #root = null; #chain = Promise.resolve(); #onError;
  constructor({storage = globalThis.navigator?.storage, onError = () => {}} = {}) { this.storage = storage; this.#onError = onError; }
  get available() { return typeof this.storage?.getDirectory === 'function'; }
  async #base() {
    if (!this.#root) this.#root = await (await this.storage.getDirectory()).getDirectoryHandle(DIRECTORY, {create: true});
    return this.#root;
  }
  async #directory(names, create) { let dir = await this.#base(); for (const name of names) dir = await dir.getDirectoryHandle(name, {create}); return dir; }
  /* Every stored file as {path, bytes: ArrayBuffer}, plus empty directories. */
  async load() {
    if (!this.available) return {files: [], directories: []};
    this.storage.persist?.().catch(() => {});
    const files = [], directories = [];
    const walk = async (dir, prefix) => {
      let empty = true;
      for await (const [name, handle] of dir.entries()) {
        empty = false; const path = prefix ? `${prefix}/${name}` : name;
        if (handle.kind === 'file') files.push({path, bytes: await (await handle.getFile()).arrayBuffer()});
        else await walk(handle, path);
      }
      if (empty && prefix) directories.push(prefix);
    };
    await walk(await this.#base(), '');
    return {files, directories};
  }
  async #write(path, bytes) {
    const names = parts(path), dir = await this.#directory(names.slice(0, -1), true);
    const writable = await (await dir.getFileHandle(names.at(-1), {create: true})).createWritable();
    await writable.write(bytes); await writable.close();
  }
  async #remove(path, recursive) {
    const names = parts(path);
    try { await (await this.#directory(names.slice(0, -1), false)).removeEntry(names.at(-1), {recursive}); }
    catch (error) { if (error?.name !== 'NotFoundError') throw error; }
  }
  async #copy(from, to) {
    const names = parts(from), parent = await this.#directory(names.slice(0, -1), false);
    let handle;
    try { handle = await parent.getFileHandle(names.at(-1)); }
    catch (error) {
      if (error?.name !== 'TypeMismatchError') throw error;
      const dir = await parent.getDirectoryHandle(names.at(-1));
      await this.#directory(parts(to), true);
      for await (const [name] of dir.entries()) await this.#copy(`${from}/${name}`, `${to}/${name}`);
      return;
    }
    await this.#write(to, await (await handle.getFile()).arrayBuffer());
  }
  async #apply(change) {
    switch (change.op) {
      case 'write': return this.#write(change.path, change.bytes);
      case 'delete': return this.#remove(change.path, false);
      case 'rmdir': return this.#remove(change.path, true);
      case 'mkdir': return void await this.#directory(parts(change.path), true);
      case 'rename':
        if (change.from && change.to) { await this.#copy(change.from, change.to); return this.#remove(change.from, true); }
        if (change.from) return this.#remove(change.from, true);
        if (change.to && change.bytes) return this.#write(change.to, change.bytes);
        return;
      default: throw new Error(`Unknown user-data operation ${change.op}`);
    }
  }
  /* Changes are applied strictly in arrival order; failures are reported, not fatal. */
  apply(change) {
    if (!this.available) return this.#chain;
    this.#chain = this.#chain.then(() => this.#apply(change)).catch(error => this.#onError(error, change));
    return this.#chain;
  }
  flush() { return this.#chain; }
  async clear() { if (!this.available) return; await this.#chain; await (await this.storage.getDirectory()).removeEntry(DIRECTORY, {recursive: true}).catch(() => {}); this.#root = null; }
}

/* Worker side: mirror Emscripten MEMFS changes under `root` as store operations.
 * Writes are sent when a written stream closes (whole-file snapshot). */
export function mirrorUserdata(FS, send, root = '/userdata') {
  const inside = path => typeof path === 'string' && path.startsWith(root + '/');
  const relative = path => path.slice(root.length + 1);
  const snapshot = path => { try { const bytes = FS.readFile(path); return bytes.buffer.byteLength === bytes.byteLength ? bytes : bytes.slice(); } catch { return null; } };
  const original = {write: FS.write, close: FS.close, rename: FS.rename, unlink: FS.unlink, rmdir: FS.rmdir, mkdir: FS.mkdir, truncate: FS.truncate, ftruncate: FS.ftruncate};
  FS.write = function (stream, ...rest) { const result = original.write.call(this, stream, ...rest); if (inside(stream?.path)) stream.moviesDirty = true; return result; };
  FS.close = function (stream) {
    const path = stream?.path, dirty = stream?.moviesDirty, result = original.close.call(this, stream);
    if (dirty && inside(path)) { const bytes = snapshot(path); if (bytes) send({op: 'write', path: relative(path), bytes}); }
    return result;
  };
  FS.rename = function (from, to) {
    const result = original.rename.call(this, from, to);
    if (inside(from) || inside(to)) {
      const bytes = !inside(from) && inside(to) ? snapshot(to) : null;
      send({op: 'rename', from: inside(from) ? relative(from) : null, to: inside(to) ? relative(to) : null, ...(bytes ? {bytes} : {})});
    }
    return result;
  };
  FS.unlink = function (path) { const result = original.unlink.call(this, path); if (inside(path)) send({op: 'delete', path: relative(path)}); return result; };
  FS.rmdir = function (path) { const result = original.rmdir.call(this, path); if (inside(path)) send({op: 'rmdir', path: relative(path)}); return result; };
  FS.mkdir = function (path, ...rest) { const result = original.mkdir.call(this, path, ...rest); if (inside(path)) send({op: 'mkdir', path: relative(path)}); return result; };
  FS.truncate = function (path, length) { const result = original.truncate.call(this, path, length); if (inside(path)) { const bytes = snapshot(path); if (bytes) send({op: 'write', path: relative(path), bytes}); } return result; };
  FS.ftruncate = function (fd, length) { const result = original.ftruncate.call(this, fd, length); const stream = FS.getStream?.(fd); if (stream && inside(stream.path)) stream.moviesDirty = true; return result; };
  return () => Object.assign(FS, original);
}

/* Restore a stored tree into MEMFS before the game starts. */
export function restoreUserdata(FS, {files = [], directories = []} = {}, root = '/userdata') {
  const ensure = dir => { let path = ''; for (const name of dir.split('/').filter(Boolean)) { path += '/' + name; try { FS.mkdir(path); } catch (error) { if (error?.errno !== 20 && error?.code !== 'EEXIST') { const node = FS.analyzePath(path); if (!node.exists) throw error; } } } };
  ensure(root);
  for (const dir of directories) ensure(`${root}/${parts(dir).join('/')}`);
  for (const {path, bytes} of files) { const names = parts(path); ensure(`${root}/${names.slice(0, -1).join('/')}`); FS.writeFile(`${root}/${names.join('/')}`, new Uint8Array(bytes)); }
  return files.length;
}
