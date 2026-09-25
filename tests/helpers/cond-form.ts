import { FormRuntime, buildFormDefinition, createInstance, openXsn, type FormDefinition } from "../../src/index.ts";
import { buildCab } from "./build-cab.ts";
import { XSF2_NS, XSF_NS } from "./manifests.ts";

export const CN = "urn:example:cond";

const SCHEMA = `<xsd:schema targetNamespace="${CN}" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:b="${CN}" elementFormDefault="qualified">
  <xsd:element name="doc"><xsd:complexType><xsd:sequence>
    <xsd:element ref="b:kind" minOccurs="0"/><xsd:element ref="b:extra" minOccurs="0"/>
  </xsd:sequence></xsd:complexType></xsd:element>
  <xsd:element name="kind" type="xsd:string"/>
  <xsd:element name="extra" type="xsd:string"/>
</xsd:schema>`;

const TEMPLATE = `<?xml version="1.0"?>
<b:doc xmlns:b="${CN}"><b:kind/><b:extra/></b:doc>`;

const VIEW = `<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:b="${CN}">
  <xsl:template match="b:doc"><html><body>
    <div>Kind <span xd:xctname="PlainText" xd:CtrlId="KIND" xd:binding="b:kind"/></div>
    <xsl:choose>
      <xsl:when test="b:kind = 'B'"><div>Details for B <span xd:xctname="PlainText" xd:CtrlId="EXTRA" xd:binding="b:extra"/></div></xsl:when>
      <xsl:otherwise><div>Nothing more is needed</div></xsl:otherwise>
    </xsl:choose>
  </body></html></xsl:template>
</xsl:stylesheet>`;

const MANIFEST = `<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.1" productVersion="15.0.0" name="urn:example:cond"
  xmlns:xsf="${XSF_NS}" xmlns:xsf2="${XSF2_NS}" xmlns:b="${CN}">
  <xsf:package><xsf:files>
    <xsf:file name="myschema.xsd"><xsf:fileProperties><xsf:property name="rootElement" type="string" value="doc"></xsf:property></xsf:fileProperties></xsf:file>
    <xsf:file name="template.xml"></xsf:file>
  </xsf:files></xsf:package>
  <xsf:documentSchemas><xsf:documentSchema rootSchema="yes" location="${CN} myschema.xsd"></xsf:documentSchema></xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Conditions" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:views default="Main"><xsf:view name="Main"><xsf:mainpane transform="view1.xsl"></xsf:mainpane></xsf:view></xsf:views>
</xsf:xDocumentClass>`;

export function condXsnBytes(): Buffer {
  return buildCab([
    { name: "manifest.xsf", data: MANIFEST },
    { name: "myschema.xsd", data: SCHEMA },
    { name: "template.xml", data: TEMPLATE },
    { name: "view1.xsl", data: VIEW },
  ]);
}

export function condFixture(): { form: FormDefinition; runtime: FormRuntime } {
  const pkg = openXsn(condXsnBytes());
  const form = buildFormDefinition(pkg);
  return { form, runtime: new FormRuntime(createInstance(pkg, form), form) };
}
