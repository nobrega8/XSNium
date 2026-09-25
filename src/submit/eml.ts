import { randomBytes } from "node:crypto";
import { safeAttachmentName } from "../data/blobs.ts";

/**
 * A draft email as a standard .eml file with the form's XML attached. XSNium never sends anything: the
 * person opens the file in their mail program, checks it and sends it themselves.
 */

export interface EmailDraft {
  to: string[];
  cc: string[];
  bcc: string[];
  subject: string;
  intro: string;
  attachmentName: string;
  attachment: Buffer;
}

const MAX_SUBJECT = 200;
const MAX_ADDRESSES = 50;
// Deliberately narrow: no whitespace, quotes, angle brackets or separators, so an address can never carry a header break.
const ADDRESS = /^[^\s@<>()[\]",;:\\]{1,64}@[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?\.[A-Za-z]{2,}$/;

/** Split a recipient list on ; or , and keep only well-formed addresses. */
export function parseAddresses(text: string): { valid: string[]; invalid: number } {
  const valid: string[] = [];
  let invalid = 0;
  for (const part of text.split(/[;,]/)) {
    const address = part.trim();
    if (address === "") continue;
    if (ADDRESS.test(address) && valid.length < MAX_ADDRESSES) valid.push(address);
    else invalid++;
  }
  return { valid, invalid };
}

/** Header text with no line breaks or control characters, encoded when it is not plain ASCII. */
function headerText(text: string): string {
  const clean = [...text].filter((c) => c.charCodeAt(0) >= 0x20 && c.charCodeAt(0) !== 0x7f).join("").trim().slice(0, MAX_SUBJECT);
  return /^[\x20-\x7e]*$/.test(clean) ? clean : `=?UTF-8?B?${Buffer.from(clean, "utf8").toString("base64")}?=`;
}

function wrap(base64: string): string {
  return base64.replace(/.{1,76}/g, "$&\r\n").trimEnd();
}

export function buildEml(draft: EmailDraft): Buffer {
  const boundary = `xsnium-${randomBytes(12).toString("hex")}`;
  const name = safeAttachmentName(draft.attachmentName.endsWith(".xml") ? draft.attachmentName : `${draft.attachmentName}.xml`);
  const lines: string[] = ["X-Unsent: 1"];
  if (draft.to.length > 0) lines.push(`To: ${draft.to.join(", ")}`);
  if (draft.cc.length > 0) lines.push(`Cc: ${draft.cc.join(", ")}`);
  if (draft.bcc.length > 0) lines.push(`Bcc: ${draft.bcc.join(", ")}`);
  lines.push(`Subject: ${headerText(draft.subject)}`, "MIME-Version: 1.0", `Content-Type: multipart/mixed; boundary="${boundary}"`, "", `--${boundary}`);
  lines.push('Content-Type: text/plain; charset="utf-8"', "Content-Transfer-Encoding: base64", "", wrap(Buffer.from(draft.intro, "utf8").toString("base64")), `--${boundary}`);
  const encodedName = /^[A-Za-z0-9._ -]+$/.test(name) ? `filename="${name}"` : `filename*=UTF-8''${encodeURIComponent(name)}`;
  lines.push(`Content-Type: application/xml; name="${/^[\x20-\x7e]*$/.test(name) ? name.replace(/"/g, "_") : "form.xml"}"`, "Content-Transfer-Encoding: base64", `Content-Disposition: attachment; ${encodedName}`, "", wrap(draft.attachment.toString("base64")), `--${boundary}--`, "");
  return Buffer.from(lines.join("\r\n"), "utf8");
}
