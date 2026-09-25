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

export function samplePackage(template: string = SAMPLE_TEMPLATE): XsnPackage {
  const manifest = SAMPLE_MANIFEST.replace(
    '<xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property>',
    '<xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property><xsf:property name="rootElement" type="string" value="root"></xsf:property>',
  );
  return openXsn(buildCab([
    { name: "manifest.xsf", data: manifest },
    { name: "myschema.xsd", data: SAMPLE_SCHEMA },
    { name: "template.xml", data: template },
  ]));
}

export function sampleForm(pkg: XsnPackage = samplePackage()): FormDefinition {
  return buildFormDefinition(pkg);
}
