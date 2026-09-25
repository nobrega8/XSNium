import { randomBytes, timingSafeEqual } from "node:crypto";
import { readFileSync } from "node:fs";
import { createServer as createHttpServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createInstance, loadInstance, type FormInstance } from "../data/instance.ts";
import { buildFormDefinition, mimeTypeOf } from "../form/build.ts";
import type { FormDefinition } from "../form/model.ts";
import { XsnError } from "../package/errors.ts";
import { readEmailSettings } from "../manifest/read.ts";
import { openXsn, type XsnPackage } from "../package/xsn-package.ts";
import { buildEml } from "../submit/eml.ts";
import { expandView, type RenderNode } from "../render/expand.ts";
import { FormRuntime, type Outcome } from "../runtime/runtime.ts";

/**
 * Local web front end. It serves the UI and a small JSON API for one open form at a time.
 *
 * It is a desktop-style tool, not a network service: it listens on the loopback interface only,
 * requires a per-run token on every API call, and rejects requests whose Host or Origin is not its own
 * (which blocks DNS rebinding and cross-site requests from other pages in the user's browser).
 */

/**
 * The page's own files. In a single-file build they are assets inside the executable; otherwise they
 * are read from the public folder next to this module.
 */
function readPublic(file: string): Buffer {
  const sea = process.getBuiltinModule("node:sea");
  if (sea.isSea()) return Buffer.from(sea.getAsset(`public/${file}`));
  return readFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)), "public", file));
}
const DEFAULT_MAX_UPLOAD_BYTES = 64 * 1024 * 1024;
const MAX_JSON_BYTES = 1024 * 1024;
const MAX_INLINE_VALUE = 20_000;

const SECURITY_HEADERS: Record<string, string> = {
  "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
  "X-Content-Type-Options": "nosniff",
  "Referrer-Policy": "no-referrer",
  "Cross-Origin-Resource-Policy": "same-origin",
  "Cache-Control": "no-store",
};

const STATIC_FILES: Record<string, { file: string; type: string }> = {
  "/app.js": { file: "app.js", type: "text/javascript; charset=utf-8" },
  "/app.css": { file: "app.css", type: "text/css; charset=utf-8" },
  "/icon.svg": { file: "icon.svg", type: "image/svg+xml" },
};

export interface ServerOptions {
  /** Port to listen on; 0 picks a free one. */
  port?: number;
  /** A form to open at start-up. */
  file?: string;
  /** Largest accepted upload, in bytes. */
  maxUploadBytes?: number;
}

export interface RunningServer {
  server: Server;
  url: string;
  token: string;
  close(): Promise<void>;
}

interface Session {
  pkg: XsnPackage;
  form: FormDefinition;
  runtime: FormRuntime;
  fileName: string;
}

/** What an edit did, with the new values of everything it changed so the page can update in place. */
function describe(outcome: Outcome, instance: FormInstance) {
  const values: Record<string, string> = {};
  for (const path of outcome.changed) {
    try {
      const value = instance.getValue(path) ?? "";
      // Pictures and attachments can be megabytes of base64; the page fetches those separately, and
      // binary fields are never inlined however small they happen to be.
      const node = instance.select(path)[0];
      const binary = node?.kind === "element" && instance.schemaNodeOf(node.el)?.type?.name === "base64Binary";
      if (!binary && value.length <= MAX_INLINE_VALUE) values[path] = value;
    } catch {
      /* a path the subset cannot address is simply not reported */
    }
  }
  return { ok: true as const, changed: outcome.changed, values, events: outcome.events, issues: outcome.issues };
}

class HttpError extends Error {
  readonly status: number;
  readonly code: string;

  constructor(status: number, code: string, message: string) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

/** Dropdowns whose options come from a secondary data source get them once the user has supplied that data. */
function fillOptions(nodes: RenderNode[], runtime: FormRuntime): void {
  for (const node of nodes) {
    const source = node.properties["optionsSource"] as Parameters<FormRuntime["optionsFrom"]>[0] | undefined;
    if (source) {
      const options = runtime.optionsFrom(source);
      if (options) {
        node.properties["options"] = options;
        node.properties["optionsLoaded"] = true;
      }
    }
    fillOptions(node.children ?? [], runtime);
    for (const row of node.rows ?? []) fillOptions(row.children, runtime);
  }
}

function summary(session: Session | undefined) {
  if (!session) return { loaded: false as const };
  const { form } = session;
  const instance = session.runtime.instance;
  return {
    loaded: true as const,
    fileName: session.fileName,
    name: form.name,
    version: form.version,
    views: form.views.map((v) => ({ name: v.name, caption: v.caption, isDefault: v.isDefault })),
    initialView: instance.initialView ?? form.views.find((v) => v.isDefault)?.name ?? form.views[0]?.name,
    features: form.features,
    diagnostics: form.diagnostics,
    secondary: session.runtime.secondarySources(),
    dataSources: form.dataSources.map((d) => ({ id: d.id, kind: d.kind, name: d.name, connection: d.connection })),
  };
}

async function readBody(req: IncomingMessage, limit: number): Promise<Buffer> {
  const declared = Number(req.headers["content-length"] ?? 0);
  if (declared > limit) throw new HttpError(413, "TOO_LARGE", `Upload exceeds ${limit} bytes`);
  const chunks: Buffer[] = [];
  let total = 0;
  for await (const chunk of req) {
    total += (chunk as Buffer).length;
    if (total > limit) throw new HttpError(413, "TOO_LARGE", `Upload exceeds ${limit} bytes`);
    chunks.push(chunk as Buffer);
  }
  return Buffer.concat(chunks);
}

async function readJson(req: IncomingMessage): Promise<Record<string, unknown>> {
  const raw = await readBody(req, MAX_JSON_BYTES);
  try {
    const value = JSON.parse(raw.toString("utf8")) as unknown;
    if (typeof value !== "object" || value === null || Array.isArray(value)) throw new Error("not an object");
    return value as Record<string, unknown>;
  } catch {
    throw new HttpError(400, "BAD_REQUEST", "Body must be a JSON object");
  }
}

const str = (v: unknown, name: string): string => {
  if (typeof v !== "string") throw new HttpError(400, "BAD_REQUEST", `"${name}" must be a string`);
  return v;
};

function send(res: ServerResponse, status: number, body: string | Buffer, headers: Record<string, string>): void {
  res.writeHead(status, { ...SECURITY_HEADERS, ...headers });
  res.end(body);
}

const sendJson = (res: ServerResponse, status: number, value: unknown) =>
  send(res, status, JSON.stringify(value), { "Content-Type": "application/json; charset=utf-8" });

function safeFileName(name: string | undefined, fallback: string): string {
  // Keep only the last path segment, drop odd characters and leading dots.
  const last = (name ?? "").split(/[\\/]/).pop() ?? "";
  const base = last.replace(/[^\w. -]+/g, "_").replace(/^\.+/, "").slice(0, 100);
  return base || fallback;
}

export async function startServer(options: ServerOptions = {}): Promise<RunningServer> {
  const token = randomBytes(32).toString("hex");
  const tokenBuffer = Buffer.from(token);
  const maxUpload = options.maxUploadBytes ?? DEFAULT_MAX_UPLOAD_BYTES;
  let session: Session | undefined;

  const indexHtml = readPublic("index.html").toString("utf8");
  const statics = new Map(Object.entries(STATIC_FILES).map(([url, f]) => [url, { body: readPublic(f.file), type: f.type }]));

  /** A runtime over some data, with calculated fields brought up to date. */
  const start = (instance: FormInstance, form: FormDefinition, pkg: XsnPackage): FormRuntime => {
    const settings = readEmailSettings(pkg);
    const runtime = new FormRuntime(instance, form, { emailSettings: (name) => settings.get(name) });
    runtime.initialize();
    return runtime;
  };

  const open = (bytes: Buffer, fileName: string): Session => {
    const pkg = openXsn(bytes);
    const form = buildFormDefinition(pkg);
    return { pkg, form, runtime: start(createInstance(pkg, form), form, pkg), fileName };
  };

  if (options.file) {
    session = open(readFileSync(options.file), path.basename(options.file));
  }

  const requireSession = (): Session => {
    if (!session) throw new HttpError(409, "NO_FORM", "No form is open");
    return session;
  };

  const server = createHttpServer((req, res) => {
    void handle(req, res).catch((err: unknown) => {
      if (res.headersSent) return void res.end();
      if (err instanceof HttpError) return sendJson(res, err.status, { error: { code: err.code, message: err.message } });
      if (err instanceof XsnError) return sendJson(res, 400, { error: { code: err.code, message: err.message } });
      // Never echo internal details (paths, stack) to the browser.
      console.error("[server]", err);
      sendJson(res, 500, { error: { code: "INTERNAL", message: "Unexpected error" } });
    });
  });

  let origin = "";

  async function handle(req: IncomingMessage, res: ServerResponse): Promise<void> {
    const host = req.headers.host ?? "";
    const port = (server.address() as AddressInfo).port;
    if (host !== `127.0.0.1:${port}` && host !== `localhost:${port}`) throw new HttpError(403, "BAD_HOST", "Unexpected Host header");

    const url = new URL(req.url ?? "/", origin);
    const method = req.method ?? "GET";

    if (method === "GET" && url.pathname === "/") {
      return send(res, 200, indexHtml.replace("__TOKEN__", token), { "Content-Type": "text/html; charset=utf-8" });
    }
    if (method === "GET" && url.pathname === "/favicon.ico") return send(res, 204, "", {});
    const asset = method === "GET" ? statics.get(url.pathname) : undefined;
    if (asset) return send(res, 200, asset.body, { "Content-Type": asset.type });

    if (!url.pathname.startsWith("/api/")) throw new HttpError(404, "NOT_FOUND", "Not found");

    // Everything below is the API: token required, and browsers must not be tricked into calling it.
    // <img> requests and downloads cannot carry headers, so the image and blob routes alone also accept the token in the query.
    const supplied = req.headers["x-xsnium-token"] ?? (method === "GET" && (url.pathname === "/api/resource" || url.pathname === "/api/blob") ? url.searchParams.get("t") : "");
    const given = Buffer.from(String(supplied ?? ""));
    if (given.length !== tokenBuffer.length || !timingSafeEqual(given, tokenBuffer)) throw new HttpError(401, "BAD_TOKEN", "Missing or wrong token");
    const requestOrigin = req.headers.origin;
    if (requestOrigin !== undefined && requestOrigin !== origin && requestOrigin !== `http://localhost:${port}`) {
      throw new HttpError(403, "BAD_ORIGIN", "Unexpected Origin");
    }

    const route = `${method} ${url.pathname}`;
    switch (route) {
      case "GET /api/state":
        return sendJson(res, 200, summary(session));

      case "POST /api/open": {
        const bytes = await readBody(req, maxUpload);
        session = open(bytes, safeFileName(String(req.headers["x-file-name"] ?? ""), "form.xsn"));
        return sendJson(res, 200, summary(session));
      }

      case "POST /api/load-data": {
        const s = requireSession();
        const next = start(loadInstance(await readBody(req, maxUpload), s.form), s.form, s.pkg);
        next.adoptSecondary(s.runtime);
        s.runtime = next;
        return sendJson(res, 200, summary(s));
      }

      case "POST /api/new": {
        const s = requireSession();
        const next = start(createInstance(s.pkg, s.form), s.form, s.pkg);
        next.adoptSecondary(s.runtime);
        s.runtime = next;
        return sendJson(res, 200, summary(s));
      }

      case "GET /api/view": {
        const s = requireSession();
        const name = url.searchParams.get("name");
        const view = s.form.views.find((v) => v.name === name) ?? s.form.views.find((v) => v.isDefault) ?? s.form.views[0];
        if (!view) throw new HttpError(404, "NO_VIEW", "The form has no views");
        const tree = expandView(view, s.runtime.instance);
        fillOptions(tree.nodes, s.runtime);
        return sendJson(res, 200, tree);
      }

      case "POST /api/secondary": {
        const s = requireSession();
        const name = decodeURIComponent(String(req.headers["x-source-name"] ?? ""));
        s.runtime.loadSecondary(name, await readBody(req, maxUpload));
        return sendJson(res, 200, summary(s));
      }

      case "POST /api/secondary/clear": {
        const s = requireSession();
        s.runtime.unloadSecondary(str((await readJson(req))["name"], "name"));
        return sendJson(res, 200, summary(s));
      }

      case "POST /api/set": {
        const s = requireSession();
        const body = await readJson(req);
        const outcome = s.runtime.setValue(str(body["path"], "path"), str(body["value"], "value"));
        return sendJson(res, 200, describe(outcome, s.runtime.instance));
      }

      case "POST /api/rows": {
        const s = requireSession();
        const body = await readJson(req);
        const target = str(body["path"], "path");
        const op = str(body["op"], "op");
        const index = typeof body["index"] === "number" ? body["index"] : undefined;
        let outcome: Outcome;
        if (op === "add") outcome = s.runtime.addRow(target, index);
        else if (op === "remove" && index !== undefined) outcome = s.runtime.removeRow(target, index);
        else if (op === "duplicate" && index !== undefined) outcome = s.runtime.duplicateRow(target, index);
        else throw new HttpError(400, "BAD_REQUEST", "Unknown row operation");
        return sendJson(res, 200, describe(outcome, s.runtime.instance));
      }

      case "POST /api/rules": {
        const s = requireSession();
        const body = await readJson(req);
        const context = typeof body["context"] === "string" ? body["context"] : undefined;
        const outcome = s.runtime.runRuleSet(str(body["ruleSet"], "ruleSet"), context);
        return sendJson(res, 200, describe(outcome, s.runtime.instance));
      }

      case "GET /api/validate": {
        const s = requireSession();
        return sendJson(res, 200, { issues: s.runtime.validate() });
      }

      case "GET /api/blob": {
        const s = requireSession();
        const path = url.searchParams.get("path") ?? "";
        const blob = s.runtime.readBlob(path);
        if (!blob) throw new HttpError(404, "NOT_FOUND", "Nothing is stored there");
        if (blob.kind === "picture") {
          // Only raster pictures recognised by their bytes; the sandbox policy keeps even a mislabelled file inert.
          return send(res, 200, blob.bytes, { "Content-Type": blob.mime, "Content-Security-Policy": "sandbox" });
        }
        if (blob.dangerous) throw new HttpError(403, "BLOCKED", "Files of this type are not offered for download");
        return send(res, 200, blob.bytes, {
          "Content-Type": "application/octet-stream",
          "Content-Disposition": `attachment; filename*=UTF-8''${encodeURIComponent(blob.fileName)}`,
          "Content-Security-Policy": "sandbox",
        });
      }

      case "POST /api/blob": {
        const s = requireSession();
        const bytes = await readBody(req, maxUpload);
        const path = decodeURIComponent(String(req.headers["x-blob-path"] ?? ""));
        const kind = String(req.headers["x-blob-kind"] ?? "");
        let outcome: Outcome;
        if (kind === "picture") outcome = s.runtime.setPicture(path, bytes);
        else if (kind === "attachment") {
          outcome = s.runtime.setAttachment(path, decodeURIComponent(String(req.headers["x-file-name"] ?? "")), bytes);
        } else throw new HttpError(400, "BAD_REQUEST", "Unknown kind of binary data");
        return sendJson(res, 200, describe(outcome, s.runtime.instance));
      }

      case "POST /api/blob/clear": {
        const s = requireSession();
        const body = await readJson(req);
        return sendJson(res, 200, describe(s.runtime.clearBlob(str(body["path"], "path")), s.runtime.instance));
      }

      case "POST /api/submit": {
        const s = requireSession();
        const body = await readJson(req);
        const adapter = typeof body["adapter"] === "string" ? body["adapter"] : undefined;
        const { draft, skipped } = s.runtime.emailDraft(adapter);
        const name = safeFileName(draft.subject, "message").replace(/\.+$/, "");
        return send(res, 200, buildEml(draft), {
          "Content-Type": "message/rfc822",
          "Content-Disposition": `attachment; filename="${name}.eml"`,
          "Content-Security-Policy": "sandbox",
          "X-Skipped-Addresses": String(skipped),
          "X-Recipients": String(draft.to.length + draft.cc.length + draft.bcc.length),
        });
      }

      case "GET /api/xml": {
        const s = requireSession();
        const name = safeFileName(s.fileName.replace(/\.xsn$/i, ""), "form") + ".xml";
        return send(res, 200, s.runtime.instance.toXml(), {
          "Content-Type": "application/xml; charset=utf-8",
          "Content-Disposition": `attachment; filename="${name}"`,
        });
      }

      case "GET /api/resource": {
        const s = requireSession();
        const name = url.searchParams.get("name") ?? "";
        const entry = s.pkg.entries.find((e) => e.name.toLowerCase() === name.toLowerCase());
        const mime = entry ? mimeTypeOf(entry.name) : "";
        // Only raster images from the package; SVG can carry script, so it is never served.
        if (!entry || entry.kind !== "image" || !mime.startsWith("image/") || mime === "image/svg+xml") {
          throw new HttpError(404, "NOT_FOUND", "No such image");
        }
        return send(res, 200, s.pkg.read(entry.name), { "Content-Type": mime });
      }

      default:
        throw new HttpError(404, "NOT_FOUND", "Not found");
    }
  }

  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(options.port ?? 0, "127.0.0.1", resolve);
  });
  const port = (server.address() as AddressInfo).port;
  origin = `http://127.0.0.1:${port}`;

  return {
    server,
    url: origin,
    token,
    close: () =>
      new Promise<void>((resolve) => {
        server.closeAllConnections();
        server.close(() => resolve());
      }),
  };
}
