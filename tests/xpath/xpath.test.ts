import assert from "node:assert/strict";
import { existsSync, readdirSync } from "node:fs";
import path from "node:path";
import { describe, it } from "node:test";
import { buildFormDefinition, openXsn, parseDataDocument } from "../../src/index.ts";
import { evaluateXPath, compile, type XPathEnv } from "../../src/xpath/evaluator.ts";
import { elementNode, numberToString, stringValue, toNumber, toStringValue, type XNode } from "../../src/xpath/nodes.ts";

const NS = { my: "urn:my", o: "urn:o", xdMath: "http://schemas.microsoft.com/office/infopath/2003/xslt/Math" };

const XML = `<my:root xmlns:my="urn:my" xmlns:o="urn:o" id="r1" kind="k">
  <my:a>1</my:a><my:a>2</my:a><my:a>3</my:a>
  <my:group name="g1"><my:b>10</my:b><my:b>20</my:b><my:c/><o:x>ox</o:x></my:group>
  <my:group name="g2"><my:b>30</my:b><my:s>Hello  World</my:s></my:group>
  <my:blank></my:blank><my:nums><my:n>4</my:n><my:n>-2</my:n><my:n>9.5</my:n></my:nums>
  <my:dates><my:d>2024-03-15</my:d><my:d>2023-01-01</my:d><my:d>2025-12-31</my:d></my:dates>
</my:root>`;

const doc = parseDataDocument(XML);
const env = (over: Partial<XPathEnv> = {}): XPathEnv => ({ doc, resolvePrefix: (p) => (NS as Record<string, string>)[p], ...over });
const root = elementNode(doc.root);

const ev = (expr: string, ctx: XNode = root, e: XPathEnv = env()) => evaluateXPath(expr, ctx, e);
const nodes = (expr: string, ctx: XNode = root) => {
  const v = ev(expr, ctx);
  assert.ok(Array.isArray(v), `${expr} is a node-set`);
  return v as XNode[];
};
const names = (expr: string, ctx?: XNode) => nodes(expr, ctx).map((n) => (n.kind === "element" ? n.el.local : n.kind === "attribute" ? `@${n.attr.local}` : n.kind));
const strings = (expr: string, ctx?: XNode) => nodes(expr, ctx).map(stringValue);
const first = (expr: string, ctx?: XNode) => nodes(expr, ctx)[0]!;

describe("location paths", () => {
  it("selects children by name, wildcard and prefix", () => {
    assert.deepEqual(strings("my:a"), ["1", "2", "3"]);
    assert.equal(nodes("*").length, 8);
    assert.deepEqual(names("my:group/o:*"), ["x"]);
    assert.deepEqual(names("my:group/*"), ["b", "b", "c", "x", "b", "s"]);
    assert.deepEqual(names("my:a[1]/../my:blank"), ["blank"]);
  });

  it("distinguishes namespaces: an unprefixed name matches only no-namespace elements", () => {
    assert.equal(nodes("a").length, 0);
    assert.equal(nodes("my:group/my:x").length, 0);
    assert.equal(nodes("my:group/o:x").length, 1);
  });

  it("supports absolute and relative paths, dot and dot-dot", () => {
    assert.deepEqual(strings("/my:root/my:a"), ["1", "2", "3"]);
    assert.deepEqual(strings("/my:root/my:group[1]/my:b/../my:b"), ["10", "20"]);
    const b = first("my:group/my:b");
    assert.deepEqual(strings("../my:b", b), ["10", "20"]);
    assert.deepEqual(strings(".", b), ["10"]);
    assert.equal(nodes("/").length, 1);
    assert.equal((nodes("/")[0] as XNode).kind, "document");
  });

  it("searches descendants with // and from anywhere", () => {
    assert.equal(nodes("//my:b").length, 3);
    assert.equal(nodes("my:group//my:b").length, 3);
    assert.equal(nodes(".//my:s").length, 1);
    assert.equal(nodes("//@name").length, 2);
    assert.deepEqual(strings("//my:group[@name='g2']/my:b"), ["30"]);
  });

  it("reads attributes, text and node()", () => {
    assert.deepEqual(strings("@id"), ["r1"]);
    assert.deepEqual(strings("@*"), ["r1", "k"]);
    assert.deepEqual(strings("my:a[2]/text()"), ["2"]);
    assert.equal(nodes("my:group[1]/node()").length, 4);
  });

  it("returns node-sets in document order without duplicates", () => {
    assert.deepEqual(strings("my:a[3] | my:a[1] | my:a[3]"), ["1", "3"]);
    assert.deepEqual(names("//my:c | //my:s | //my:b"), ["b", "b", "c", "b", "s"]);
  });
});

describe("axes", () => {
  const b2 = () => nodes("my:group[1]/my:b")[1]!;

  it("walks up and down", () => {
    assert.deepEqual(names("ancestor::*", b2()), ["root", "group"]);
    assert.deepEqual(names("ancestor-or-self::*", b2()), ["root", "group", "b"]);
    assert.deepEqual(names("descendant::my:b"), ["b", "b", "b"]);
    assert.deepEqual(names("descendant-or-self::my:group", first("my:group")), ["group"]);
    assert.deepEqual(names("self::my:b", b2()), ["b"]);
    assert.deepEqual(names("parent::*", b2()), ["group"]);
  });

  it("walks siblings, and position counts from the context node outwards on reverse axes", () => {
    assert.deepEqual(strings("following-sibling::my:b", nodes("my:group[1]/my:b")[0]!), ["20"]);
    assert.deepEqual(strings("preceding-sibling::my:a[1]", nodes("my:a")[2]!), ["2"], "nearest preceding sibling is position 1");
    assert.deepEqual(strings("preceding-sibling::my:a[2]", nodes("my:a")[2]!), ["1"]);
    assert.equal(ev("count(preceding-sibling::*)", nodes("my:a")[2]!), 2);
  });

  it("walks following and preceding without ancestors or descendants", () => {
    const c = first("my:group[1]/my:c");
    assert.deepEqual(names("following::my:b", c), ["b"]);
    assert.deepEqual(strings("following::my:b", c), ["30"]);
    assert.deepEqual(strings("preceding::my:b", c), ["10", "20"]);
    assert.deepEqual(names("preceding::my:group", c), []);
  });

  it("rejects the namespace axis and unknown axes", () => {
    assert.throws(() => ev("namespace::*"), { code: "UNSUPPORTED_EXPRESSION" });
    assert.throws(() => ev("sideways::*"), { code: "UNSUPPORTED_EXPRESSION" });
  });
});

describe("predicates", () => {
  it("filters by position, last() and boolean tests", () => {
    assert.deepEqual(strings("my:a[2]"), ["2"]);
    assert.deepEqual(strings("my:a[last()]"), ["3"]);
    assert.deepEqual(strings("my:a[position() > 1]"), ["2", "3"]);
    assert.deepEqual(strings("my:a[. > 1 and . < 3]"), ["2"]);
    assert.deepEqual(strings("my:group[my:s]/@name"), ["g2"]);
    assert.deepEqual(strings("my:group[not(my:s)]/@name"), ["g1"]);
  });

  it("applies predicates to filter expressions, in document order", () => {
    assert.deepEqual(strings("(//my:b)[2]"), ["20"]);
    assert.deepEqual(strings("(//my:b)[last()]"), ["30"]);
    assert.deepEqual(strings("(my:a | my:nums/my:n)[4]"), ["4"]);
  });

  it("chains and nests predicates", () => {
    assert.deepEqual(strings("my:group[@name='g1']/my:b[2]"), ["20"]);
    assert.deepEqual(strings("my:a[my:a or true()][1]"), ["1"]);
    assert.deepEqual(strings("//my:b[. > ../my:b[1]]"), ["20"]);
  });
});

describe("operators", () => {
  it("does arithmetic with the right precedence", () => {
    assert.equal(ev("1 + 2 * 3"), 7);
    assert.equal(ev("(1 + 2) * 3"), 9);
    assert.equal(ev("10 div 4"), 2.5);
    assert.equal(ev("10 mod 3"), 1);
    assert.equal(ev("-5 mod 3"), -2);
    assert.equal(ev("2 - -3"), 5);
    assert.equal(ev("- 2 * 3"), -6);
    assert.equal(ev("my:a[1] + my:a[2]"), 3);
    assert.equal(ev("my:a[1]*my:a[3]"), 3, "* after a name is multiplication");
    assert.equal(ev("count(*) div 2"), 4);
  });

  it("treats operator names as names where a step is expected", () => {
    const d = parseDataDocument(`<div><mod>1</mod><and>2</and></div>`);
    const e: XPathEnv = { doc: d, resolvePrefix: () => undefined };
    assert.equal(evaluateXPath("mod + and", elementNode(d.root), e), 3);
    assert.equal(evaluateXPath("count(*) div 2", elementNode(d.root), e), 1);
  });

  it("compares values by type, and node-sets existentially", () => {
    assert.equal(ev("1 = 1.0"), true);
    assert.equal(ev("'a' = 'a'"), true);
    assert.equal(ev("'1' = 1"), true);
    assert.equal(ev("true() = 'x'"), true);
    assert.equal(ev("my:a = 2"), true);
    assert.equal(ev("my:a != 2"), true, "some a differs from 2");
    assert.equal(ev("my:a = 9"), false);
    assert.equal(ev("my:a > 2"), true);
    assert.equal(ev("my:a > 3"), false);
    assert.equal(ev("my:group/my:b = my:a"), false);
    assert.equal(ev("my:nums/my:n = my:a"), false);
    assert.equal(ev("my:blank = ''"), true);
    assert.equal(ev("my:missing = ''"), false, "an empty node-set equals nothing");
    assert.equal(ev("my:missing = false()"), true);
    assert.equal(ev("'abc' < 5"), false, "NaN compares false");
  });

  it("short-circuits and/or", () => {
    assert.equal(ev("true() or nosuchfunction()"), true);
    assert.equal(ev("false() and nosuchfunction()"), false);
    assert.throws(() => ev("false() or nosuchfunction()"), { code: "UNSUPPORTED_EXPRESSION" });
  });

  it("handles division by zero and NaN like XPath", () => {
    assert.equal(ev("1 div 0"), Number.POSITIVE_INFINITY);
    assert.equal(ev("-1 div 0"), Number.NEGATIVE_INFINITY);
    assert.ok(Number.isNaN(ev("0 div 0") as number));
    assert.ok(Number.isNaN(ev("number('x')") as number));
  });
});

describe("core functions", () => {
  it("strings", () => {
    assert.equal(ev("concat('a', 'b', 1)"), "ab1");
    assert.equal(ev("contains('hello', 'ell')"), true);
    assert.equal(ev("starts-with('hello', 'he')"), true);
    assert.equal(ev("substring-before('a-b-c', '-')"), "a");
    assert.equal(ev("substring-after('a-b-c', '-')"), "b-c");
    assert.equal(ev("substring('12345', 2, 3)"), "234");
    assert.equal(ev("substring('12345', 2)"), "2345");
    assert.equal(ev("substring('12345', 0)"), "12345");
    assert.equal(ev("substring('12345', 1.5, 2.6)"), "234");
    assert.equal(ev("string-length('héllo')"), 5);
    assert.equal(ev("normalize-space(my:group[2]/my:s)"), "Hello World");
    assert.equal(ev("translate('2024-03-15', '-', '')"), "20240315");
    assert.equal(ev("translate('abc', 'abc', 'AB')"), "AB");
    assert.equal(ev("string(my:a[2])"), "2");
    assert.equal(ev("string(12.50)"), "12.5");
  });

  it("numbers and booleans", () => {
    assert.equal(ev("sum(my:a)"), 6);
    assert.equal(ev("sum(my:nums/my:n)"), 11.5);
    assert.equal(ev("count(//my:b)"), 3);
    assert.equal(ev("floor(2.7)"), 2);
    assert.equal(ev("ceiling(2.1)"), 3);
    assert.equal(ev("round(2.5)"), 3);
    assert.equal(ev("round(-2.5)"), -2);
    assert.equal(ev("number(' 42 ')"), 42);
    assert.equal(ev("boolean(my:a)"), true);
    assert.equal(ev("boolean(my:zzz)"), false);
    assert.equal(ev("not(1 = 2)"), true);
  });

  it("names", () => {
    assert.equal(ev("local-name(my:group)"), "group");
    assert.equal(ev("name(my:group)"), "my:group");
    assert.equal(ev("namespace-uri(my:group)"), "urn:my");
    assert.equal(ev("local-name(.)", first("my:group")), "group");
    assert.equal(ev("local-name(@id)"), "id");
  });

  it("checks arity", () => {
    assert.throws(() => ev("contains('a')"), { code: "UNSUPPORTED_EXPRESSION" });
    assert.throws(() => ev("concat('a')"), { code: "UNSUPPORTED_EXPRESSION" });
    assert.throws(() => ev("count(1)"), { code: "UNSUPPORTED_EXPRESSION" });
  });
});

describe("InfoPath functions", () => {
  const clock = () => new Date(2024, 4, 9, 13, 45, 7);
  const at = (expr: string) => ev(expr, root, env({ now: clock }));

  it("xdMath:Nz turns blanks into zero or a default", () => {
    assert.equal(toNumber(ev("xdMath:Nz(my:blank)")), 0);
    assert.equal(toNumber(ev("xdMath:Nz(my:missing)")), 0);
    assert.equal(ev("xdMath:Nz(my:missing, 'n/a')"), "n/a");
    assert.equal(toStringValue(ev("xdMath:Nz(my:a[2])")), "2");
    assert.equal(ev("xdMath:Nz(my:a[2]) * 5"), 10);
    assert.equal(ev("xdMath:Nz(my:blank) + xdMath:Nz(my:a[1])"), 1);
    assert.equal(ev("xdMath:Nz('   ')"), 0);
    assert.equal(ev("sum(xdMath:Nz(my:blank | my:a))"), 6, "blank nodes count as zero, so sum() does not become NaN");
    assert.equal(ev("sum(xdMath:Nz(my:missing))"), 0);
    assert.equal(ev("xdMath:Nz(my:blank, 7) + xdMath:Nz(my:a[1], 7)"), 8);
  });

  it("xdMath:Eval with Min, Max, Avg and Sum", () => {
    assert.equal(ev("xdMath:Max(xdMath:Eval(my:nums/my:n, 'number(.)'))"), 9.5);
    assert.equal(ev("xdMath:Min(xdMath:Eval(my:nums/my:n, 'number(.)'))"), -2);
    assert.equal(ev("xdMath:Avg(my:nums/my:n)"), 11.5 / 3);
    assert.equal(ev("xdMath:Sum(my:nums/my:n)"), 11.5);
    assert.equal(ev("xdMath:Min(xdMath:Eval(my:dates/my:d, 'translate(., \"-\", \"\")'))"), 20230101);
    assert.ok(Number.isNaN(ev("xdMath:Max(my:missing)") as number), "an empty set has no maximum");
  });

  it("date functions use the clock they are given", () => {
    assert.equal(at("xdDate:Today()"), "2024-05-09");
    assert.equal(at("xdDate:Now()"), "2024-05-09T13:45:07");
    assert.equal(at("xdDate:AddDays(xdDate:Today(), 30)"), "2024-06-08");
    assert.equal(at("xdDate:AddDays('2024-02-28', 2)"), "2024-03-01");
    assert.equal(at("xdDate:AddDays('2024-03-01', -1)"), "2024-02-29");
    assert.equal(at("xdDate:AddSeconds('2024-05-09T23:59:30', 45)"), "2024-05-10T00:00:15");
    assert.equal(at("xdDate:AddDays('not a date', 1)"), "");
  });

  it("compares dates as the calculations in real forms do", () => {
    assert.equal(at("msxsl:string-compare(xdDate:Today(), '2024-05-10')"), -1);
    assert.equal(at("msxsl:string-compare('2024-05-09', xdDate:Today())"), 0);
    assert.equal(at("msxsl:string-compare('b', 'a')"), 1);
    assert.equal(at("msxsl:string-compare('A', 'a', '', 'i')"), 0);
    assert.equal(at("my:dates/my:d[msxsl:string-compare(., xdDate:Today()) > 0]").constructor, Array);
    assert.deepEqual(strings("my:dates/my:d[msxsl:string-compare(., '2024-01-01') > 0]"), ["2024-03-15", "2025-12-31"]);
  });

  it("other functions are empty or false rather than reaching outside the form", () => {
    assert.deepEqual(ev("xdXDocument:GetDOM('Some source')"), []);
    assert.equal(ev("count(xdXDocument:GetDOM('Some source')/anything)"), 0);
    assert.equal(ev("xdEnvironment:IsBrowser()"), false);
  });

  it("uses the conventional prefix when the form does not declare it", () => {
    const e = env({ resolvePrefix: (p) => (p === "my" ? "urn:my" : undefined) });
    assert.equal(toNumber(evaluateXPath("xdMath:Nz(my:blank)", root, e)), 0);
  });

  it("refuses unknown functions and prefixes instead of guessing", () => {
    assert.throws(() => ev("xdUtil:Match('a', 'b')"), { code: "UNSUPPORTED_EXPRESSION", message: /xdUtil:Match/ });
    assert.throws(() => ev("nosuch:thing()"), { code: "UNSUPPORTED_EXPRESSION" });
    assert.throws(() => ev("zz:a"), { code: "UNSUPPORTED_EXPRESSION", message: /zz/ });
    assert.throws(() => ev("frobnicate(1)"), { code: "UNSUPPORTED_EXPRESSION" });
  });
});

describe("number and string conversion", () => {
  it("formats numbers without exponents, as XPath does", () => {
    assert.equal(numberToString(0), "0");
    assert.equal(numberToString(-0), "0");
    assert.equal(numberToString(3), "3");
    assert.equal(numberToString(0.5), "0.5");
    assert.equal(numberToString(1e21), "1000000000000000000000");
    assert.equal(numberToString(0.0000001), "0.0000001");
    assert.equal(numberToString(Number.NaN), "NaN");
    assert.equal(numberToString(Number.NEGATIVE_INFINITY), "-Infinity");
    assert.equal(toStringValue(ev("0.1 + 0.2")), "0.30000000000000004");
  });

  it("parses numbers strictly", () => {
    assert.ok(Number.isNaN(ev("number('1e3')") as number), "XPath 1.0 has no exponent notation");
    assert.ok(Number.isNaN(ev("number('')") as number));
    assert.ok(Number.isNaN(ev("number('0x10')") as number));
    assert.equal(ev("number('.5')"), 0.5);
    assert.equal(ev("number('-3.')"), -3);
  });
});

describe("hostile and malformed expressions", () => {
  for (const bad of ["", "   ", "//", "a[", "a]", "a[1", "(", ")", "1 +", "a b", "@", "a::", "'unterminated", "$var", "a/[1]", "1 2", "../", "a |", "child::", "/@", "position(", "f(,)", "f(1,)"]) {
    it(`reports ${JSON.stringify(bad)} as an unsupported expression, never anything else`, () => {
      assert.throws(() => compile(bad), (err: unknown) => (err as { code?: string }).code === "UNSUPPORTED_EXPRESSION");
    });
  }

  it("caps expression length and nesting", () => {
    assert.throws(() => compile("a".repeat(9000)), { code: "UNSUPPORTED_EXPRESSION" });
    assert.throws(() => compile("(".repeat(200) + "1" + ")".repeat(200)), { code: "UNSUPPORTED_EXPRESSION" });
    assert.throws(() => compile("-".repeat(200) + "1"), { code: "UNSUPPORTED_EXPRESSION" });
    assert.doesNotThrow(() => compile("(".repeat(30) + "1" + ")".repeat(30)));
  });

  it("stops expensive expressions", () => {
    const wide = parseDataDocument(`<r>${"<i><j><k/><k/><k/></j><j><k/><k/></j></i>".repeat(200)}</r>`);
    const e: XPathEnv = { doc: wide, resolvePrefix: () => undefined, maxSteps: 5_000 };
    assert.throws(() => evaluateXPath("//*//*//*//*", elementNode(wide.root), e), { code: "LIMIT_EXCEEDED" });
    assert.throws(() => evaluateXPath("count(//*[count(//*) > 0])", elementNode(wide.root), e), { code: "LIMIT_EXCEEDED" });
  });

  it("stops an expression that keeps evaluating itself through xdMath:Eval", () => {
    const loop = parseDataDocument(`<r>xdMath:Eval(., string(.))</r>`);
    const e: XPathEnv = { doc: loop, resolvePrefix: (p) => (p === "xdMath" ? NS.xdMath : undefined) };
    assert.throws(() => evaluateXPath("xdMath:Eval(., string(.))", elementNode(loop.root), e), { code: "UNSUPPORTED_EXPRESSION", message: /nested too deeply/ });
  });

  it("cannot reach anything outside the data it is given", () => {
    for (const expr of ["document('file:///etc/passwd')", "system-property('xsl:version')", "unparsed-entity-uri('x')", "key('k','v')", "generate-id()", "current()"]) {
      assert.throws(() => ev(expr), { code: "UNSUPPORTED_EXPRESSION" }, expr);
    }
  });
});

const fixtureDir = path.resolve("example_files");
const fixtures = existsSync(fixtureDir) ? readdirSync(fixtureDir).filter((f) => f.toLowerCase().endsWith(".xsn")) : [];

describe("real-world expressions", { skip: fixtures.length === 0 && "no local fixtures present" }, () => {
  for (const file of fixtures) {
    it(`parses every calculation, rule and validation expression of fixture #${fixtures.indexOf(file) + 1}`, () => {
      const form = buildFormDefinition(openXsn(path.join(fixtureDir, file)));
      const expressions: string[] = [];
      for (const r of form.rules) {
        if (r.condition) expressions.push(r.condition);
        for (const a of r.actions) if (a.type === "setValue") expressions.push(a.expression);
      }
      for (const v of form.validations) if (v.type === "custom" && v.expression) expressions.push(v.expression);
      for (const e of expressions) assert.doesNotThrow(() => compile(e), e);
    });
  }
});

describe("function-available", () => {
  it("tells a template which functions it may use", () => {
    assert.equal(ev("function-available('count')"), true);
    assert.equal(ev("function-available('xdMath:Eval')"), true);
    assert.equal(ev("function-available('xdMath:NoSuchThing')"), false);
    assert.equal(ev("function-available('nosuchfunction')"), false);
    assert.equal(ev("function-available('unknownprefix:foo')"), false);
  });
});
