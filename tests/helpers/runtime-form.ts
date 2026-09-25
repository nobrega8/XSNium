import { FormRuntime, buildFormDefinition, createInstance, openXsn, type FormDefinition } from "../../src/index.ts";
import { buildCab } from "./build-cab.ts";
import { XSF2_NS, XSF_NS } from "./manifests.ts";

export const RT = "urn:example:runtime";

/** A form with calculations, rules, validation and a repeating table, for exercising the runtime. */
const SCHEMA = `<xsd:schema targetNamespace="${RT}" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:r="${RT}" elementFormDefault="qualified">
  <xsd:element name="order"><xsd:complexType><xsd:sequence>
    <xsd:element ref="r:name"/>
    <xsd:element ref="r:qty" minOccurs="0"/>
    <xsd:element ref="r:price" minOccurs="0"/>
    <xsd:element ref="r:total" minOccurs="0"/>
    <xsd:element ref="r:tax" minOccurs="0"/>
    <xsd:element ref="r:grand" minOccurs="0"/>
    <xsd:element ref="r:status" minOccurs="0"/>
    <xsd:element ref="r:note" minOccurs="0"/>
    <xsd:element ref="r:code" minOccurs="0"/>
    <xsd:element ref="r:flag" minOccurs="0"/>
    <xsd:element ref="r:when" minOccurs="0"/>
    <xsd:element ref="r:ratio" minOccurs="0"/>
    <xsd:element ref="r:lines" minOccurs="0" maxOccurs="unbounded"/>
    <xsd:element ref="r:linesTotal" minOccurs="0"/>
    <xsd:element ref="r:ping" minOccurs="0"/>
    <xsd:element ref="r:pong" minOccurs="0"/>
    <xsd:element ref="r:a" minOccurs="0"/>
    <xsd:element ref="r:b" minOccurs="0"/>
  </xsd:sequence></xsd:complexType></xsd:element>
  <xsd:element name="name" type="xsd:string"/>
  <xsd:element name="qty" type="xsd:integer"/>
  <xsd:element name="price" type="xsd:decimal"/>
  <xsd:element name="total" type="xsd:double"/>
  <xsd:element name="tax" type="xsd:double"/>
  <xsd:element name="grand" type="xsd:double"/>
  <xsd:element name="status"><xsd:simpleType><xsd:restriction base="xsd:string"><xsd:enumeration value="open"/><xsd:enumeration value="closed"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="note"><xsd:simpleType><xsd:restriction base="xsd:string"><xsd:maxLength value="5"/><xsd:minLength value="2"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="code"><xsd:simpleType><xsd:restriction base="xsd:string"><xsd:pattern value="[A-Z]{3}"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="flag" type="xsd:string"/>
  <xsd:element name="when" type="xsd:date"/>
  <xsd:element name="ratio"><xsd:simpleType><xsd:restriction base="xsd:decimal"><xsd:minInclusive value="0"/><xsd:maxExclusive value="10"/><xsd:totalDigits value="4"/><xsd:fractionDigits value="2"/></xsd:restriction></xsd:simpleType></xsd:element>
  <xsd:element name="lines"><xsd:complexType><xsd:sequence>
    <xsd:element name="amount" type="xsd:double" minOccurs="0"/>
    <xsd:element name="count" type="xsd:integer" minOccurs="0"/>
    <xsd:element name="lineTotal" type="xsd:double" minOccurs="0"/>
  </xsd:sequence><xsd:attribute name="id" type="xsd:string" use="required"/></xsd:complexType></xsd:element>
  <xsd:element name="linesTotal" type="xsd:double"/>
  <xsd:element name="ping" type="xsd:string"/><xsd:element name="pong" type="xsd:string"/>
  <xsd:element name="a" type="xsd:double"/><xsd:element name="b" type="xsd:double"/>
</xsd:schema>`;

const TEMPLATE = `<?xml version="1.0"?>
<r:order xmlns:r="${RT}"><r:name>Acme</r:name><r:qty>2</r:qty><r:price>10.50</r:price><r:total/><r:tax/><r:grand/><r:status>open</r:status><r:note/><r:code/><r:flag/><r:ratio/><r:ping/><r:pong/><r:a/><r:b/>
  <r:lines id="L1"><r:amount>5</r:amount><r:count>2</r:count><r:lineTotal/></r:lines>
  <r:lines id="L2"><r:amount>7</r:amount><r:count>3</r:count><r:lineTotal/></r:lines>
  <r:linesTotal/></r:order>`;

const RULES_AND_MORE = `
  <xsf:calculations>
    <xsf:calculatedField target="/r:order/r:grand" expression="xdMath:Nz(../r:total) + xdMath:Nz(../r:tax)" refresh="onChange"></xsf:calculatedField>
    <xsf:calculatedField target="/r:order/r:total" expression="xdMath:Nz(../r:qty) * xdMath:Nz(../r:price)" refresh="onChange"></xsf:calculatedField>
    <xsf:calculatedField target="/r:order/r:tax" expression="round(xdMath:Nz(../r:total) div 10)" refresh="onChange"></xsf:calculatedField>
    <xsf:calculatedField target="/r:order/r:lines/r:lineTotal" expression="xdMath:Nz(../r:amount) * xdMath:Nz(../r:count)" refresh="onChange"></xsf:calculatedField>
    <xsf:calculatedField target="/r:order/r:linesTotal" expression="sum(xdMath:Nz(../r:lines/r:lineTotal))" refresh="onChange"></xsf:calculatedField>
  </xsf:calculations>
  <xsf:ruleSets>
    <xsf:ruleSet name="onStatus">
      <xsf:rule caption="closed" condition=". = &quot;closed&quot;"><xsf:assignmentAction targetField="../r:note" expression="&quot;done&quot;"></xsf:assignmentAction></xsf:rule>
      <xsf:rule caption="reopened" condition=". != &quot;closed&quot;"><xsf:assignmentAction targetField="../r:note" expression="&quot;&quot;"></xsf:assignmentAction></xsf:rule>
      <xsf:rule caption="disabled" isEnabled="no"><xsf:assignmentAction targetField="../r:flag" expression="&quot;never&quot;"></xsf:assignmentAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="onNote">
      <xsf:rule caption="cascade" condition=". = &quot;done&quot;"><xsf:assignmentAction targetField="../r:code" expression="&quot;ABC&quot;"></xsf:assignmentAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="onCode">
      <xsf:rule caption="flag from code"><xsf:assignmentAction targetField="../r:flag" expression="concat(&quot;code:&quot;, .)"></xsf:assignmentAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="onPing"><xsf:rule caption="ping"><xsf:assignmentAction targetField="../r:pong" expression="concat(., &quot;+&quot;)"></xsf:assignmentAction></xsf:rule></xsf:ruleSet>
    <xsf:ruleSet name="onPong"><xsf:rule caption="pong"><xsf:assignmentAction targetField="../r:ping" expression="concat(., &quot;-&quot;)"></xsf:assignmentAction></xsf:rule></xsf:ruleSet>
    <xsf:ruleSet name="broken">
      <xsf:rule caption="bad function"><xsf:assignmentAction targetField="../r:flag" expression="nosuchfunction(.)"></xsf:assignmentAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="onQty"><xsf:rule caption="uses broken"><xsf:assignmentAction targetField="../r:flag" expression="unknown:fn(.)"></xsf:assignmentAction></xsf:rule></xsf:ruleSet>
    <xsf:ruleSet name="buttonRules">
      <xsf:rule caption="reset"><xsf:assignmentAction targetField="r:status" expression="&quot;open&quot;"></xsf:assignmentAction></xsf:rule>
      <xsf:rule caption="view"><xsf:switchViewAction view="Second"></xsf:switchViewAction></xsf:rule>
      <xsf:rule caption="send"><xsf:submitAction adapter="Main"></xsf:submitAction></xsf:rule>
      <xsf:rule caption="ask"><xsf:dialogBoxMessageAction>hi</xsf:dialogBoxMessageAction></xsf:rule>
    </xsf:ruleSet>
    <xsf:ruleSet name="rowButton"><xsf:rule caption="row"><xsf:assignmentAction targetField="r:amount" expression="99"></xsf:assignmentAction></xsf:rule></xsf:ruleSet>
  </xsf:ruleSets>
  <xsf:domEventHandlers>
    <xsf:domEventHandler match="/r:order/r:status"><xsf:ruleSetAction ruleSet="onStatus"></xsf:ruleSetAction></xsf:domEventHandler>
    <xsf:domEventHandler match="/r:order/r:note"><xsf:ruleSetAction ruleSet="onNote"></xsf:ruleSetAction></xsf:domEventHandler>
    <xsf:domEventHandler match="/r:order/r:code"><xsf:ruleSetAction ruleSet="onCode"></xsf:ruleSetAction></xsf:domEventHandler>
    <xsf:domEventHandler match="/r:order/r:ping"><xsf:ruleSetAction ruleSet="onPing"></xsf:ruleSetAction></xsf:domEventHandler>
    <xsf:domEventHandler match="/r:order/r:pong"><xsf:ruleSetAction ruleSet="onPong"></xsf:ruleSetAction></xsf:domEventHandler>
    <xsf:domEventHandler match="/r:order/r:qty"><xsf:ruleSetAction ruleSet="onQty"></xsf:ruleSetAction></xsf:domEventHandler>
  </xsf:domEventHandlers>
  <xsf:customValidation>
    <xsf:errorCondition match="/r:order/r:when" expressionContext="." expression="msxsl:string-compare(., xdDate:Today()) &gt; 0">
      <xsf:errorMessage type="modeless" shortMessage="The date cannot be in the future"></xsf:errorMessage>
    </xsf:errorCondition>
  </xsf:customValidation>`;

function manifest(extra: string): string {
  return `<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.1" productVersion="15.0.0" name="urn:example:runtime"
  xmlns:xsf="${XSF_NS}" xmlns:xsf2="${XSF2_NS}" xmlns:r="${RT}" xmlns:xdMath="http://schemas.microsoft.com/office/infopath/2003/xslt/Math" xmlns:xdDate="http://schemas.microsoft.com/office/infopath/2003/xslt/Date">
  <xsf:package><xsf:files>
    <xsf:file name="myschema.xsd"><xsf:fileProperties><xsf:property name="rootElement" type="string" value="order"></xsf:property></xsf:fileProperties></xsf:file>
    <xsf:file name="template.xml"></xsf:file>
  </xsf:files></xsf:package>
  <xsf:documentSchemas><xsf:documentSchema rootSchema="yes" location="${RT} myschema.xsd"></xsf:documentSchema></xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Runtime" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:views default="First"><xsf:view name="First"><xsf:mainpane transform="view1.xsl"></xsf:mainpane></xsf:view><xsf:view name="Second"><xsf:mainpane transform="view2.xsl"></xsf:mainpane></xsf:view></xsf:views>
  ${extra}
</xsf:xDocumentClass>`;
}

/** Two calculated fields that depend on each other, so they never settle. */
const CYCLE_CALCS = [
  '<xsf:calculatedField target="/r:order/r:a" expression="xdMath:Nz(../r:b) + 1" refresh="onChange"></xsf:calculatedField>',
  '<xsf:calculatedField target="/r:order/r:b" expression="xdMath:Nz(../r:a) + 1" refresh="onChange"></xsf:calculatedField>',
].join("");

export function runtimeXsnBytes(template: string = TEMPLATE, options: { cycle?: boolean } = {}): Buffer {
  // The manifest allows one calculations block, so the extra fields go inside it.
  const rules = options.cycle ? RULES_AND_MORE.replace("</xsf:calculations>", `${CYCLE_CALCS}</xsf:calculations>`) : RULES_AND_MORE;
  return buildCab([
    { name: "manifest.xsf", data: manifest(rules) },
    { name: "myschema.xsd", data: SCHEMA },
    { name: "template.xml", data: template },
  ]);
}

export interface RuntimeFixture {
  form: FormDefinition;
  runtime: FormRuntime;
}

/** A runtime over the form above, with a fixed clock. */
export function runtimeFixture(template?: string, options: { cycle?: boolean } = {}): RuntimeFixture {
  const pkg = openXsn(runtimeXsnBytes(template, options));
  const form = buildFormDefinition(pkg);
  const instance = createInstance(pkg, form);
  const runtime = new FormRuntime(instance, form, { now: () => new Date(2024, 4, 9, 12, 0, 0) });
  return { form, runtime };
}
