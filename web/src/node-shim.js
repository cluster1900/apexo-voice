// A small in-memory stand-in for the Node APIs that the sherpa-onnx npm wasm build calls.
//
// The package's wasm is built for Node (emscripten NODERAWFS): it requires "path", "fs" and "crypto" and reads
// model files through `fs`. A browser has none of that, so the worker hands the glue code these shims instead.
// Only what loading and running a model needs is implemented; anything else fails with a clear error code.

function fsError(code, path) {
  const error = new Error(`${code}: ${path ?? ""}`);
  error.code = code;
  return error;
}

function normalize(path) {
  const absolute = path.startsWith("/");
  const parts = [];
  for (const part of path.split("/")) {
    if (!part || part === ".") continue;
    if (part === "..") parts.pop();
    else parts.push(part);
  }
  return (absolute ? "/" : "") + parts.join("/") || (absolute ? "/" : ".");
}

const pathShim = {
  isAbsolute: (path) => path.startsWith("/"),
  normalize,
  dirname(path) {
    const clean = normalize(path);
    const index = clean.lastIndexOf("/");
    if (index < 0) return ".";
    return index === 0 ? "/" : clean.slice(0, index);
  },
  basename(path) {
    const clean = normalize(path);
    return clean.slice(clean.lastIndexOf("/") + 1);
  },
  join: (...parts) => normalize(parts.filter(Boolean).join("/")),
};
pathShim.posix = pathShim;

// Linux values; the glue only forwards them to openSync.
const OPEN_FLAGS = {
  O_APPEND: 1024, O_CREAT: 64, O_EXCL: 128, O_NOCTTY: 256, O_NOFOLLOW: 131072, O_RDONLY: 0, O_RDWR: 2,
  O_SYNC: 1052672, O_TRUNC: 512, O_WRONLY: 1, O_DSYNC: 4096, O_NONBLOCK: 2048, O_DIRECTORY: 65536,
};

// `files` maps absolute paths to Uint8Array contents. Directories are implied by the file paths plus "/".
export function createNodeShim(files) {
  const directories = new Set(["/"]);
  const addParents = (path) => {
    for (let dir = pathShim.dirname(path); !directories.has(dir); dir = pathShim.dirname(dir)) directories.add(dir);
  };
  for (const path of files.keys()) addParents(path);

  const descriptors = new Map();
  let nextFd = 3;

  function stat(path) {
    const clean = normalize(path);
    if (directories.has(clean)) return makeStat(true, 0, clean);
    const bytes = files.get(clean);
    if (!bytes) throw fsError("ENOENT", path);
    return makeStat(false, bytes.length, clean);
  }

  function makeStat(isDir, size, key) {
    let ino = 1;
    for (let i = 0; i < key.length; i += 1) ino = (ino * 31 + key.charCodeAt(i)) >>> 0;
    const time = new Date(0);
    return {
      dev: 1, mode: isDir ? 0o40755 : 0o100644, nlink: 1, uid: 0, gid: 0, rdev: 0, size, blksize: 4096,
      blocks: Math.ceil(size / 512), atimeMs: 0, mtimeMs: 0, ctimeMs: 0, birthtimeMs: 0, ino,
      atime: time, mtime: time, ctime: time, birthtime: time,
      isDirectory: () => isDir, isFile: () => !isDir, isSymbolicLink: () => false,
    };
  }

  const fs = {
    statSync: stat,
    lstatSync: stat,
    fstatSync(fd) {
      const entry = descriptors.get(fd);
      if (!entry) throw fsError("EBADF");
      return stat(entry.path);
    },
    openSync(path, flags = 0) {
      const clean = normalize(path);
      if (directories.has(clean)) {
        const fd = nextFd++;
        descriptors.set(fd, { path: clean, position: 0 });
        return fd;
      }
      if (!files.has(clean)) {
        if (!(flags & OPEN_FLAGS.O_CREAT)) throw fsError("ENOENT", path);
        files.set(clean, new Uint8Array(0));
        addParents(clean);
      }
      const fd = nextFd++;
      descriptors.set(fd, { path: clean, position: 0 });
      return fd;
    },
    closeSync(fd) {
      descriptors.delete(fd);
    },
    readSync(fd, buffer, offset, length, position) {
      const entry = descriptors.get(fd);
      if (!entry) throw fsError("EBADF");
      const bytes = files.get(entry.path);
      if (!bytes) throw fsError("EISDIR", entry.path);
      const from = typeof position === "number" ? position : entry.position;
      const count = Math.max(0, Math.min(length, bytes.length - from));
      if (count > 0) buffer.set(bytes.subarray(from, from + count), offset);
      if (typeof position !== "number") entry.position += count;
      return count;
    },
    writeSync(fd, buffer, offset = 0, length = buffer.length - offset) {
      if (fd === 1 || fd === 2) return length; // stdout and stderr are dropped
      throw fsError("EROFS");
    },
    readFileSync(path) {
      const bytes = files.get(normalize(path));
      if (!bytes) throw fsError("ENOENT", path);
      return bytes;
    },
    existsSync: (path) => directories.has(normalize(path)) || files.has(normalize(path)),
    mkdirSync(path) {
      const clean = normalize(path);
      directories.add(clean);
      addParents(clean);
    },
    readdirSync(path) {
      const clean = normalize(path);
      if (!directories.has(clean)) throw fsError("ENOENT", path);
      const prefix = clean === "/" ? "/" : `${clean}/`;
      const names = new Set();
      for (const key of [...files.keys(), ...directories]) {
        if (key !== clean && key.startsWith(prefix)) names.add(key.slice(prefix.length).split("/")[0]);
      }
      return [...names];
    },
    unlinkSync(path) {
      files.delete(normalize(path));
    },
  };
  const unsupported = (name) => () => {
    throw fsError("EPERM", name);
  };
  for (const name of [
    "chmodSync", "chownSync", "fchmodSync", "fchownSync", "ftruncateSync", "futimesSync", "readlinkSync",
    "renameSync", "rmdirSync", "statfsSync", "symlinkSync", "truncateSync", "utimesSync", "writeFileSync",
  ]) {
    fs[name] = unsupported(name);
  }

  let cwd = "/";
  const process = {
    versions: { node: "20.0.0" },
    platform: "linux",
    argv: ["node", "/sherpa-onnx"],
    env: {},
    exitCode: undefined,
    stdin: { fd: 0 },
    cwd: () => cwd,
    chdir(path) {
      cwd = normalize(path);
    },
    binding(name) {
      if (name === "constants") return { fs: OPEN_FLAGS };
      throw fsError("ENOSYS", name);
    },
    on() {},
  };

  const crypto = {
    randomFillSync(view) {
      globalThis.crypto.getRandomValues(new Uint8Array(view.buffer, view.byteOffset, view.byteLength));
      return view;
    },
  };

  function require(name) {
    if (name === "fs") return fs;
    if (name === "path") return pathShim;
    if (name === "crypto") return crypto;
    throw fsError("MODULE_NOT_FOUND", name);
  }

  return {
    require,
    process,
    addFile(path, bytes) {
      const clean = normalize(path);
      files.set(clean, bytes);
      addParents(clean);
    },
    removeFile(path) {
      files.delete(normalize(path));
    },
  };
}
