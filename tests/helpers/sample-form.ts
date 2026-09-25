import { buildFormDefinition, openXsn, type FormDefinition, type XsnPackage } from "../../src/index.ts";
import { buildCab } from "./build-cab.ts";
import { SAMPLE_MANIFEST } from "./manifests.ts";

export const MY = "urn:example:my";

export const SAMPLE_SCHEMA = `<xsd:schema targetNamespace="${MY}" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:my="${MY}"
    elementFormDefault="qualified">
  <xsd:element name="root"><xsd:complexType><xsd:sequence>
    <xsd:element ref="my:title"/>
    <xsd:element ref="my:note" minOccurs="0"/>
    <xsd:element ref="my:items" minOccurs="0" maxOccurs="unbounded"/>
    <xsd:element ref="my:limited" maxOccurs="2"/>
    <xsd:element ref="my:late" minOccurs="0"/>
  </xsd:sequence><xsd:attribute name="version" type="xsd:string" default="1"/></xsd:complexType></xsd:element>
  <xsd:element name="title" type="xsd:string"/>
  <xsd:element name="note" type="xsd:string" nillable="true"/>
  <xsd:element name="items"><xsd:complexType><xsd:sequence>
    <xsd:element name="name" type="xsd:string"/>
    <xsd:element name="qty" type="xsd:integer" minOccurs="0"/>
  </xsd:sequence><xsd:attribute name="id" type="xsd:string" use="required"/></xsd:complexType></xsd:element>
  <xsd:element name="limited" type="xsd:string"/>
  <xsd:element name="late" type="xsd:string"/>
</xsd:schema>`;

export const SAMPLE_TEMPLATE = `<?xml version="1.0" encoding="UTF-8"?>
<?mso-infoPathSolution name="urn:example:form" href="manifest.xsf" solutionVersion="1.0.0.7" ?>
<?mso-application progid="InfoPath.Document"?>
<my:root xmlns:my="${MY}" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" version="1">
	<my:title>Hello</my:title>
	<my:note xsi:nil="true"/>
	<my:items id="a"><my:name>first</my:name><my:qty>2</my:qty></my:items>
	<my:limited>x</my:limited>
</my:root>`;

/** A small but realistic view: labels, a text field, a dropdown, radios and a repeating table. */
export const SAMPLE_VIEW = `<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:my="${MY}">
  <xsl:template match="my:root"><html><body>
    <div>Title <span xd:xctname="PlainText" xd:CtrlId="TITLE" xd:binding="my:title"><xsl:value-of select="my:title"/></span></div>
    <div>Note <select xd:xctname="dropdown" xd:CtrlId="NOTE" xd:binding="my:note"><option>Select...</option><option value="low">Low</option><option value="high">High</option></select></div>
    <div>Late
      <input type="radio" xd:xctname="OptionButton" xd:CtrlId="R1" xd:binding="my:late" xd:onValue="yes"/> Yes
      <input type="radio" xd:xctname="OptionButton" xd:CtrlId="R2" xd:binding="my:late" xd:onValue="no"/> No
    </div>
    <table><thead><tr><td>Item name</td></tr></thead>
      <tbody xd:xctname="RepeatingTable"><xsl:for-each select="my:items"><tr><td><span xd:xctname="PlainText" xd:CtrlId="NAME" xd:binding="my:name"><xsl:value-of select="my:name"/></span></td></tr></xsl:for-each></tbody>
    </table>
  </body></html></xsl:template>
</xsl:stylesheet>`;

/** The sample form as raw .xsn bytes, optionally with extra package files. */
export function sampleXsnBytes(extra: { name: string; data: Buffer | string }[] = []): Buffer {
  const manifest = SAMPLE_MANIFEST.replace(
    '<xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property>',
    '<xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property><xsf:property name="rootElement" type="string" value="root"></xsf:property>',
  );
  return buildCab([
    { name: "manifest.xsf", data: manifest },
    { name: "myschema.xsd", data: SAMPLE_SCHEMA },
    { name: "template.xml", data: SAMPLE_TEMPLATE },
    { name: "view1.xsl", data: SAMPLE_VIEW },
    { name: "view2.xsl", data: SAMPLE_VIEW },
    ...extra,
  ]);
}

export function samplePackage(template: string = SAMPLE_TEMPLATE, manifestExtra = ""): XsnPackage {
  const manifest = SAMPLE_MANIFEST.replace(
    '<xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property>',
    '<xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property><xsf:property name="rootElement" type="string" value="root"></xsf:property>',
  ).replace("</xsf:xDocumentClass>", `${manifestExtra}</xsf:xDocumentClass>`);
  return openXsn(buildCab([
    { name: "manifest.xsf", data: manifest },
    { name: "myschema.xsd", data: SAMPLE_SCHEMA },
    { name: "template.xml", data: template },
  ]));
}

export function sampleForm(pkg: XsnPackage = samplePackage()): FormDefinition {
  return buildFormDefinition(pkg);
}
