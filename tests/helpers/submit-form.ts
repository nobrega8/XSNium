import { FormRuntime, buildFormDefinition, createInstance, openXsn, readEmailSettings, type FormDefinition } from "../../src/index.ts";
import { buildCab } from "./build-cab.ts";
import { XSF2_NS, XSF_NS } from "./manifests.ts";

export const SB = "urn:example:submit";

const SCHEMA = `<xsd:schema targetNamespace="${SB}" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:b="${SB}" elementFormDefault="qualified">
  <xsd:element name="doc"><xsd:complexType><xsd:sequence>
    <xsd:element ref="b:manager" minOccurs="0"/><xsd:element ref="b:title" minOccurs="0"/>
  </xsd:sequence></xsd:complexType></xsd:element>
  <xsd:element name="manager" type="xsd:string"/>
  <xsd:element name="title" type="xsd:string"/>
</xsd:schema>`;

const TEMPLATE = `<?xml version="1.0"?>
<b:doc xmlns:b="${SB}"><b:manager>boss@example.invalid</b:manager><b:title>Quarterly report</b:title></b:doc>`;

const VIEW = `<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:b="${SB}">
  <xsl:template match="b:doc"><html><body>
    <div>Title <span xd:xctname="PlainText" xd:CtrlId="TITLE" xd:binding="b:title"/></div>
    <div><input type="button" value="Submit" xd:xctname="Button" xd:CtrlId="SUBMIT" xd:action="submit"/></div>
  </body></html></xsl:template>
</xsl:stylesheet>`;

const MANIFEST = `<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.1" productVersion="15.0.0" name="urn:example:submit"
  xmlns:xsf="${XSF_NS}" xmlns:xsf2="${XSF2_NS}" xmlns:b="${SB}">
  <xsf:package><xsf:files>
    <xsf:file name="myschema.xsd"><xsf:fileProperties><xsf:property name="rootElement" type="string" value="doc"></xsf:property></xsf:fileProperties></xsf:file>
    <xsf:file name="template.xml"></xsf:file>
  </xsf:files></xsf:package>
  <xsf:documentSchemas><xsf:documentSchema rootSchema="yes" location="${SB} myschema.xsd"></xsf:documentSchema></xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Submit" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:submit caption="Submit" onAfterSubmit="keepOpen"><xsf:emailAdapter name="Send" submitAllowed="yes">
    <xsf:to value="b:manager" valueType="expression"></xsf:to>
    <xsf:cc value="team@example.invalid; not an address; second@example.invalid"></xsf:cc>
    <xsf:subject value="concat('Report: ', b:title)" valueType="expression"></xsf:subject>
    <xsf:intro value="Please review the attached form."></xsf:intro>
    <xsf:attachmentFileName value="Report" valueType="literal"></xsf:attachmentFileName>
  </xsf:emailAdapter></xsf:submit>
  <xsf:views default="Main"><xsf:view name="Main"><xsf:mainpane transform="view1.xsl"></xsf:mainpane></xsf:view></xsf:views>
</xsf:xDocumentClass>`;

export function submitXsnBytes(): Buffer {
  return buildCab([
    { name: "manifest.xsf", data: MANIFEST },
    { name: "myschema.xsd", data: SCHEMA },
    { name: "template.xml", data: TEMPLATE },
    { name: "view1.xsl", data: VIEW },
  ]);
}

export function submitFixture(): { form: FormDefinition; runtime: FormRuntime } {
  const pkg = openXsn(submitXsnBytes());
  const form = buildFormDefinition(pkg);
  const settings = readEmailSettings(pkg);
  return { form, runtime: new FormRuntime(createInstance(pkg, form), form, { emailSettings: (name) => settings.get(name) }) };
}
