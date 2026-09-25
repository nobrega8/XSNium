export type XsnErrorCode =
  | "NOT_A_CABINET"
  | "TRUNCATED"
  | "MALFORMED"
  | "UNSUPPORTED_COMPRESSION"
  | "UNSUPPORTED_MULTI_CABINET"
  | "LIMIT_EXCEEDED"
  | "UNSAFE_PATH"
  | "ENTRY_NOT_FOUND";

export class XsnError extends Error {
  readonly code: XsnErrorCode;

  constructor(code: XsnErrorCode, message: string) {
    super(message);
    this.name = "XsnError";
    this.code = code;
  }
}
