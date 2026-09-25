/**
 * Lexical and facet checks for values of the XSD types templates use. Only what the schema states is
 * enforced: unknown types and unsupported facets pass rather than reject.
 */

const INTEGER_RANGES: Record<string, [bigint | undefined, bigint | undefined]> = {
  integer: [undefined, undefined],
  long: [-(2n ** 63n), 2n ** 63n - 1n],
  int: [-(2n ** 31n), 2n ** 31n - 1n],
  short: [-32768n, 32767n],
  byte: [-128n, 127n],
  nonNegativeInteger: [0n, undefined],
  positiveInteger: [1n, undefined],
  nonPositiveInteger: [undefined, 0n],
  negativeInteger: [undefined, -1n],
  unsignedLong: [0n, 2n ** 64n - 1n],
  unsignedInt: [0n, 2n ** 32n - 1n],
  unsignedShort: [0n, 65535n],
  unsignedByte: [0n, 255n],
};

const DECIMAL = /^[+-]?(\d+(\.\d*)?|\.\d+)$/;
const DOUBLE = /^[+-]?(\d+(\.\d*)?|\.\d+)([eE][+-]?\d+)?$|^[+-]?INF$|^NaN$/;
const ZONE = "(Z|[+-]\\d{2}:\\d{2})?";
const DATE = new RegExp(`^(-?\\d{4,})-(\\d{2})-(\\d{2})${ZONE}$`);
const TIME = new RegExp(`^(\\d{2}):(\\d{2}):(\\d{2})(\\.\\d+)?${ZONE}$`);
const DATETIME = new RegExp(`^(-?\\d{4,})-(\\d{2})-(\\d{2})T(\\d{2}):(\\d{2}):(\\d{2})(\\.\\d+)?${ZONE}$`);

const daysIn = (year: number, month: number) => new Date(Date.UTC(year, month, 0)).getUTCDate();

function validDate(y: string, m: string, d: string): boolean {
  const month = Number(m);
  return month >= 1 && month <= 12 && Number(d) >= 1 && Number(d) <= daysIn(Math.abs(Number(y)) || 2000, month);
}

const validTime = (h: string, m: string, s: string) => Number(h) <= 24 && Number(m) <= 59 && Number(s) <= 59;

/** Types compared as numbers when checking bounds. */
export function isNumericType(type: string): boolean {
  return type in INTEGER_RANGES || type === "decimal" || type === "double" || type === "float";
}

export function isDateType(type: string): boolean {
  return type === "date" || type === "dateTime" || type === "time";
}

/** A message when `value` is not a valid lexical form of `type`, otherwise undefined. */
export function checkType(type: string, raw: string): string | undefined {
  const value = raw.trim();
  if (type in INTEGER_RANGES) {
    if (!/^[+-]?\d+$/.test(value)) return "Enter a whole number";
    const [min, max] = INTEGER_RANGES[type]!;
    const n = BigInt(value);
    if ((min !== undefined && n < min) || (max !== undefined && n > max)) return "The number is out of range";
    return undefined;
  }
  switch (type) {
    case "decimal":
      return DECIMAL.test(value) ? undefined : "Enter a number";
    case "double":
    case "float":
      return DOUBLE.test(value) ? undefined : "Enter a number";
    case "boolean":
      return ["true", "false", "1", "0"].includes(value) ? undefined : "Enter true or false";
    case "date": {
      const m = DATE.exec(value);
      return m && validDate(m[1]!, m[2]!, m[3]!) ? undefined : "Enter a date (YYYY-MM-DD)";
    }
    case "time": {
      const m = TIME.exec(value);
      return m && validTime(m[1]!, m[2]!, m[3]!) ? undefined : "Enter a time (hh:mm:ss)";
    }
    case "dateTime": {
      const m = DATETIME.exec(value);
      return m && validDate(m[1]!, m[2]!, m[3]!) && validTime(m[4]!, m[5]!, m[6]!) ? undefined : "Enter a date and time (YYYY-MM-DDThh:mm:ss)";
    }
    case "base64Binary":
      return /^[A-Za-z0-9+/\s=]*$/.test(value) ? undefined : "Not valid base64 data";
    default:
      return undefined;
  }
}

const MAX_PATTERN_LENGTH = 300;
const MAX_CHECKED_LENGTH = 2000;

/**
 * XSD patterns are anchored regular expressions. They come from untrusted templates and JavaScript cannot
 * interrupt a regular expression, so patterns that could backtrack catastrophically are not run.
 */
export function checkPattern(pattern: string, value: string): "match" | "mismatch" | "skipped" {
  if (pattern.length > MAX_PATTERN_LENGTH || value.length > MAX_CHECKED_LENGTH) return "skipped";
  // A quantified group that itself contains a quantifier, or alternation inside a quantified group.
  if (/\([^)]*[+*}][^)]*\)\s*[+*{]/.test(pattern) || /\([^)]*\|[^)]*\)\s*[+*{]/.test(pattern)) return "skipped";
  let re: RegExp;
  try {
    re = new RegExp(`^(?:${pattern})$`, "u");
  } catch {
    return "skipped";
  }
  return re.test(value) ? "match" : "mismatch";
}

/** Digits before and after the decimal point, ignoring sign and insignificant zeros. */
export function digitCounts(value: string): { total: number; fraction: number } {
  const [whole = "", frac = ""] = value.trim().replace(/^[+-]/, "").split(".");
  const w = whole.replace(/^0+(?=\d)/, "");
  const f = frac.replace(/0+$/, "");
  return { total: (w === "0" ? 0 : w.length) + f.length, fraction: f.length };
}
