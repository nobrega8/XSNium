import { inflateRawSync } from "node:zlib";
import { XsnError } from "./errors.ts";
import { DEFAULT_LIMITS, type PackageLimits } from "./limits.ts";

/**
 * Minimal, read-only Microsoft Cabinet (CAB) reader.
 *
 * An .xsn is a CAB archive, not a ZIP. Supports uncompressed and MSZIP folders,
 * which is what InfoPath produces. Multi-cabinet sets, LZX and Quantum are
 * reported as unsupported rather than guessed at.
 */

const SIGNATURE = 0x4643534d; // "MSCF"
const HEADER_SIZE = 36;
const FLAG_PREV_CABINET = 0x0001;
const FLAG_NEXT_CABINET = 0x0002;
const FLAG_RESERVE_PRESENT = 0x0004;
const ATTR_NAME_IS_UTF = 0x80;
const MSZIP_BLOCK_MAX = 32768;

const COMPRESSION_NONE = 0;
const COMPRESSION_MSZIP = 1;

export interface CabFileEntry {
  /** Name exactly as stored in the cabinet (unsanitised). */
  rawName: string;
  size: number;
  modified: Date | undefined;
  folderIndex: number;
  offsetInFolder: number;
}

interface CabFolder {
  dataOffset: number;
  dataBlocks: number;
  compression: number;
}

export interface CabArchive {
  files: CabFileEntry[];
  extract(entry: CabFileEntry): Buffer;
}

function need(buf: Buffer, offset: number, length: number, what: string): void {
  if (offset < 0 || offset + length > buf.length) {
    throw new XsnError("TRUNCATED", `Cabinet truncated while reading ${what}`);
  }
}

function readCString(buf: Buffer, offset: number, utf8: boolean, what: string): { value: string; next: number } {
  let end = offset;
  while (end < buf.length && buf[end] !== 0) end++;
  if (end >= buf.length) throw new XsnError("TRUNCATED", `Unterminated string in ${what}`);
  return { value: buf.toString(utf8 ? "utf8" : "latin1", offset, end), next: end + 1 };
}

function dosDateTime(date: number, time: number): Date | undefined {
  const day = date & 0x1f;
  const month = (date >> 5) & 0x0f;
  const year = 1980 + (date >> 9);
  if (day === 0 || month === 0 || month > 12) return undefined;
  return new Date(Date.UTC(year, month - 1, day, (time >> 11) & 0x1f, (time >> 5) & 0x3f, (time & 0x1f) * 2));
}

export function isCabinet(buf: Buffer): boolean {
  return buf.length >= 4 && buf.readUInt32LE(0) === SIGNATURE;
}

export function openCab(buf: Buffer, limits: PackageLimits = DEFAULT_LIMITS): CabArchive {
  if (buf.length > limits.maxPackageBytes) {
    throw new XsnError("LIMIT_EXCEEDED", `Package is ${buf.length} bytes, limit is ${limits.maxPackageBytes}`);
  }
  if (!isCabinet(buf)) throw new XsnError("NOT_A_CABINET", "Not a Cabinet file (missing MSCF signature)");
  need(buf, 0, HEADER_SIZE, "header");

  const coffFiles = buf.readUInt32LE(16);
  const folderCount = buf.readUInt16LE(26);
  const fileCount = buf.readUInt16LE(28);
  const flags = buf.readUInt16LE(30);

  if (flags & (FLAG_PREV_CABINET | FLAG_NEXT_CABINET)) {
    throw new XsnError("UNSUPPORTED_MULTI_CABINET", "Multi-cabinet sets are not supported");
  }
  if (fileCount > limits.maxEntries) {
    throw new XsnError("LIMIT_EXCEEDED", `Package declares ${fileCount} entries, limit is ${limits.maxEntries}`);
  }

  let pos = HEADER_SIZE;
  let folderReserve = 0;
  let dataReserve = 0;
  if (flags & FLAG_RESERVE_PRESENT) {
    need(buf, pos, 4, "reserve header");
    const headerReserve = buf.readUInt16LE(pos);
    folderReserve = buf[pos + 2]!;
    dataReserve = buf[pos + 3]!;
    pos += 4 + headerReserve;
  }

  const folders: CabFolder[] = [];
  for (let i = 0; i < folderCount; i++) {
    need(buf, pos, 8 + folderReserve, "folder table");
    const compressionType = buf.readUInt16LE(pos + 6) & 0x0f;
    if (compressionType !== COMPRESSION_NONE && compressionType !== COMPRESSION_MSZIP) {
      throw new XsnError("UNSUPPORTED_COMPRESSION", `Folder ${i} uses unsupported compression type ${compressionType}`);
    }
    folders.push({
      dataOffset: buf.readUInt32LE(pos),
      dataBlocks: buf.readUInt16LE(pos + 4),
      compression: compressionType,
    });
    pos += 8 + folderReserve;
  }

  const files: CabFileEntry[] = [];
  pos = coffFiles;
  let declaredTotal = 0;
  for (let i = 0; i < fileCount; i++) {
    need(buf, pos, 16, "file table");
    const size = buf.readUInt32LE(pos);
    const offsetInFolder = buf.readUInt32LE(pos + 4);
    const folderIndex = buf.readUInt16LE(pos + 8);
    const date = buf.readUInt16LE(pos + 10);
    const time = buf.readUInt16LE(pos + 12);
    const attribs = buf.readUInt16LE(pos + 14);
    const name = readCString(buf, pos + 16, (attribs & ATTR_NAME_IS_UTF) !== 0, "file table");
    pos = name.next;

    if (folderIndex >= folders.length) {
      throw new XsnError("MALFORMED", `Entry "${name.value}" references missing folder ${folderIndex}`);
    }
    if (size > limits.maxEntryBytes) {
      throw new XsnError("LIMIT_EXCEEDED", `Entry "${name.value}" is ${size} bytes, limit is ${limits.maxEntryBytes}`);
    }
    declaredTotal += size;
    if (declaredTotal > limits.maxTotalBytes) {
      throw new XsnError("LIMIT_EXCEEDED", `Package expands beyond ${limits.maxTotalBytes} bytes`);
    }
    files.push({ rawName: name.value, size, modified: dosDateTime(date, time), folderIndex, offsetInFolder });
  }

  const folderCache = new Map<number, Buffer>();

  function readFolder(index: number): Buffer {
    const cached = folderCache.get(index);
    if (cached) return cached;
    const folder = folders[index]!;
    const chunks: Buffer[] = [];
    let total = 0;
    let previous: Buffer | undefined;
    let p = folder.dataOffset;
    for (let b = 0; b < folder.dataBlocks; b++) {
      need(buf, p, 8 + dataReserve, "data block header");
      const compressedLen = buf.readUInt16LE(p + 4);
      const uncompressedLen = buf.readUInt16LE(p + 6);
      p += 8 + dataReserve;
      need(buf, p, compressedLen, "data block");
      const payload = buf.subarray(p, p + compressedLen);
      p += compressedLen;

      let block: Buffer;
      if (folder.compression === COMPRESSION_NONE) {
        block = Buffer.from(payload);
      } else {
        if (payload.length < 2 || payload[0] !== 0x43 || payload[1] !== 0x4b) {
          throw new XsnError("MALFORMED", "MSZIP block is missing the CK signature");
        }
        try {
          block = inflateRawSync(payload.subarray(2), {
            // Each block may reference the previous one as its dictionary.
            ...(previous ? { dictionary: previous } : {}),
            maxOutputLength: MSZIP_BLOCK_MAX,
          });
        } catch (cause) {
          throw new XsnError("MALFORMED", `Corrupt MSZIP block: ${(cause as Error).message}`);
        }
      }
      if (block.length !== uncompressedLen) {
        throw new XsnError("MALFORMED", "Data block size does not match its declared size");
      }
      total += block.length;
      if (total > limits.maxTotalBytes) {
        throw new XsnError("LIMIT_EXCEEDED", `Folder expands beyond ${limits.maxTotalBytes} bytes`);
      }
      chunks.push(block);
      previous = block;
    }
    const data = Buffer.concat(chunks);
    folderCache.set(index, data);
    return data;
  }

  return {
    files,
    extract(entry) {
      const data = readFolder(entry.folderIndex);
      if (entry.offsetInFolder + entry.size > data.length) {
        throw new XsnError("MALFORMED", `Entry "${entry.rawName}" extends past the end of its folder`);
      }
      return Buffer.from(data.subarray(entry.offsetInFolder, entry.offsetInFolder + entry.size));
    },
  };
}
