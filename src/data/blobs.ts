import { XsnError } from "../package/errors.ts";

/**
 * Binary data stored inside form data as base64: embedded pictures and file attachments
 * (MS-IPFFX 2.1.3 and 2.1.4). Everything here treats the bytes as untrusted: sizes are bounded, headers are
 * checked field by field, names are sanitised, and nothing is ever written to disk or executed.
 */

/** Largest decoded binary value accepted in one field. */
export const MAX_BLOB_BYTES = 32 * 1024 * 1024;

const ATTACHMENT_SIGNATURE = Buffer.from([0xc7, 0x49, 0x46, 0x41]);
const HEADER_SIZE = 20;
const MAX_FILE_NAME_CHARS = 260;

/** Extensions the specification forbids in an attachment's file name. */
const DANGEROUS_EXTENSIONS = new Set(
  ("ade adp app asp bas bat cer chm cmd com cpl crt csh exe fxp gadget hlp hta inf ins isp its js jse ksh lnk mad maf mag mam maq mar mas mat mau mav maw mda mdb mde mdt mdw mdz msc msi msp mst ops pcd pif prf prg ps1 ps1xml ps2 ps2xml psc1 psc2 pst reg scf scr sct shb shs tmp url vb vbe vbs vsmacros vss vst vsw ws wsc wsf wsh").split(" "),
);

export type ImageType = "png" | "jpeg" | "gif" | "bmp";
export const IMAGE_MIME: Record<ImageType, string> = { png: "image/png", jpeg: "image/jpeg", gif: "image/gif", bmp: "image/bmp" };

/** Decode a base64 field. Whitespace is allowed (InfoPath wraps long values); anything else invalid is refused. */
export function decodeBase64(text: string): Buffer {
  const compact = text.replace(/\s+/g, "");
  if (compact.length > Math.ceil((MAX_BLOB_BYTES * 4) / 3) + 4) throw new XsnError("LIMIT_EXCEEDED", "Binary value is too large");
  if (!/^[A-Za-z0-9+/]*={0,2}$/.test(compact) || compact.length % 4 === 1) throw new XsnError("MALFORMED", "Not valid base64 data");
  return Buffer.from(compact, "base64");
}

export function encodeBase64(bytes: Buffer): string {
  if (bytes.length > MAX_BLOB_BYTES) throw new XsnError("LIMIT_EXCEEDED", `Binary value exceeds ${MAX_BLOB_BYTES} bytes`);
  return bytes.toString("base64");
}

/** Recognise a picture by its first bytes. Only raster formats a browser shows safely are accepted (never SVG or HTML). */
export function sniffImage(bytes: Buffer): ImageType | undefined {
  if (bytes.length >= 8 && bytes.subarray(0, 8).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]))) return "png";
  if (bytes.length >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff) return "jpeg";
  if (bytes.length >= 6 && (bytes.toString("latin1", 0, 6) === "GIF87a" || bytes.toString("latin1", 0, 6) === "GIF89a")) return "gif";
  if (bytes.length >= 26 && bytes[0] === 0x42 && bytes[1] === 0x4d) return "bmp";
  return undefined;
}

export function isDangerousFileName(name: string): boolean {
  const dot = name.lastIndexOf(".");
  return dot >= 0 && DANGEROUS_EXTENSIONS.has(name.slice(dot + 1).toLowerCase());
}

/** A file name safe to show and to offer for download: no path, no control characters, bounded length. */
export function safeAttachmentName(name: string): string {
  const last = name.split(/[\\/]/).pop() ?? "";
  // eslint-disable-next-line no-control-regex
  const clean = last.replace(/[\u0000-\u001f\u007f<>:"|?*]+/g, "_").replace(/^\.+/, "").trim().slice(0, MAX_FILE_NAME_CHARS - 1);
  return clean === "" ? "attachment" : clean;
}

export interface Attachment {
  fileName: string;
  bytes: Buffer;
}

/** Read an attachment structure. Throws MALFORMED with the field that is wrong. */
export function parseAttachment(data: Buffer): Attachment {
  const bad = (why: string): never => {
    throw new XsnError("MALFORMED", `Not a valid file attachment: ${why}`);
  };
  if (data.length < HEADER_SIZE + 4) bad("too short");
  if (!data.subarray(0, 4).equals(ATTACHMENT_SIGNATURE)) bad("wrong signature");
  const headerSize = data.readUInt32LE(4);
  if (headerSize !== HEADER_SIZE) bad(`unexpected header size ${headerSize}`);
  if (data.readUInt32LE(8) !== 1) bad("unknown version");
  const fileSize = data.readUInt32LE(16);
  const nameChars = data.readUInt32LE(20);
  if (fileSize > MAX_BLOB_BYTES) throw new XsnError("LIMIT_EXCEEDED", "Attachment is too large");
  if (nameChars <= 1 || nameChars > MAX_FILE_NAME_CHARS) bad("file name length out of range");
  const nameEnd = 24 + nameChars * 2;
  if (data.length < nameEnd) bad("truncated file name");
  if (data.readUInt16LE(nameEnd - 2) !== 0) bad("file name is not terminated");
  const fileName = data.toString("utf16le", 24, nameEnd - 2);
  if (fileName.includes("\u0000")) bad("file name contains a NUL");
  const bytes = data.subarray(nameEnd);
  if (bytes.length !== fileSize) bad("file size does not match the data");
  return { fileName, bytes: Buffer.from(bytes) };
}

/** Build an attachment structure for a file. */
export function buildAttachment(fileName: string, bytes: Buffer): Buffer {
  if (bytes.length > MAX_BLOB_BYTES) throw new XsnError("LIMIT_EXCEEDED", `Attachment exceeds ${MAX_BLOB_BYTES} bytes`);
  const name = safeAttachmentName(fileName);
  if (isDangerousFileName(name)) throw new XsnError("INVALID_OPERATION", `Files of this type cannot be attached: ${name}`);
  const nameBytes = Buffer.from(`${name}\u0000`, "utf16le");
  const header = Buffer.alloc(24);
  ATTACHMENT_SIGNATURE.copy(header, 0);
  header.writeUInt32LE(HEADER_SIZE, 4);
  header.writeUInt32LE(1, 8);
  header.writeUInt32LE(0, 12);
  header.writeUInt32LE(bytes.length, 16);
  header.writeUInt32LE(nameBytes.length / 2, 20);
  return Buffer.concat([header, nameBytes, bytes]);
}

/** What is stored in a binary field, without keeping the bytes around. */
export type BlobInfo =
  | { kind: "empty" }
  | { kind: "picture"; type: ImageType; mime: string; size: number }
  | { kind: "attachment"; fileName: string; size: number; dangerous: boolean }
  | { kind: "unknown"; size: number };

export function describeBlob(base64: string): BlobInfo {
  if (base64.trim() === "") return { kind: "empty" };
  let bytes: Buffer;
  try {
    bytes = decodeBase64(base64);
  } catch {
    return { kind: "unknown", size: 0 };
  }
  const image = sniffImage(bytes);
  if (image) return { kind: "picture", type: image, mime: IMAGE_MIME[image], size: bytes.length };
  if (bytes.subarray(0, 4).equals(ATTACHMENT_SIGNATURE)) {
    try {
      const a = parseAttachment(bytes);
      return { kind: "attachment", fileName: safeAttachmentName(a.fileName), size: a.bytes.length, dangerous: isDangerousFileName(a.fileName) };
    } catch {
      return { kind: "unknown", size: bytes.length };
    }
  }
  return { kind: "unknown", size: bytes.length };
}
