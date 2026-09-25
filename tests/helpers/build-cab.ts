import { deflateRawSync } from "node:zlib";

export interface TestFile {
  name: string;
  data: Buffer | string;
}

/** Builds a single-folder CAB. MSZIP chains each 32K block as the next one's dictionary. */
export function buildCab(files: TestFile[], opts: { mszip?: boolean } = {}): Buffer {
  const mszip = opts.mszip ?? true;
  const contents = files.map((f) => (typeof f.data === "string" ? Buffer.from(f.data) : f.data));
  const folderData = Buffer.concat(contents);

  const blocks: Buffer[] = [];
  let prev: Buffer | undefined;
  for (let off = 0; off < Math.max(folderData.length, 1); off += 32768) {
    const raw = folderData.subarray(off, off + 32768);
    const payload = mszip
      ? Buffer.concat([Buffer.from("CK"), deflateRawSync(raw, prev ? { dictionary: prev } : {})])
      : raw;
    const header = Buffer.alloc(8);
    header.writeUInt16LE(payload.length, 4);
    header.writeUInt16LE(raw.length, 6);
    blocks.push(Buffer.concat([header, payload]));
    prev = Buffer.from(raw);
  }

  const fileTable: Buffer[] = [];
  let offset = 0;
  files.forEach((f, i) => {
    const size = contents[i]!.length;
    const fixed = Buffer.alloc(16);
    fixed.writeUInt32LE(size, 0);
    fixed.writeUInt32LE(offset, 4);
    fixed.writeUInt16LE(0, 8);
    fixed.writeUInt16LE(((2025 - 1980) << 9) | (2 << 5) | 18, 10);
    fixed.writeUInt16LE(12 << 11, 12);
    fileTable.push(fixed, Buffer.from(f.name + "\0", "latin1"));
    offset += size;
  });
  const fileBytes = Buffer.concat(fileTable);

  const coffFiles = 36 + 8;
  const coffData = coffFiles + fileBytes.length;
  const dataBytes = Buffer.concat(blocks);

  const header = Buffer.alloc(36);
  header.write("MSCF", 0, "latin1");
  header.writeUInt32LE(coffData + dataBytes.length, 8);
  header.writeUInt32LE(coffFiles, 16);
  header[24] = 3;
  header[25] = 1;
  header.writeUInt16LE(1, 26);
  header.writeUInt16LE(files.length, 28);

  const folder = Buffer.alloc(8);
  folder.writeUInt32LE(coffData, 0);
  folder.writeUInt16LE(blocks.length, 4);
  folder.writeUInt16LE(mszip ? 1 : 0, 6);

  return Buffer.concat([header, folder, fileBytes, dataBytes]);
}
