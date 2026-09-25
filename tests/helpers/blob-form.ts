import { FormRuntime, buildFormDefinition, createInstance, openXsn, type FormDefinition } from "../../src/index.ts";
import { buildCab } from "./build-cab.ts";
import { XSF2_NS, XSF_NS } from "./manifests.ts";

export const BL = "urn:example:blobs";

/** A real 1x1 PNG, so a browser can actually decode it. */
export const TINY_PNG = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==", "base64");

const SCHEMA = `<xsd:schema targetNamespace="${BL}" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:b="${BL}" elementFormDefault="qualified">
  <xsd:element name="doc"><xsd:complexType><xsd:sequence>
    <xsd:element ref="b:title"/><xsd:element ref="b:photo" minOccurs="0"/><xsd:element ref="b:file" minOccurs="0"/><xsd:element ref="b:note" minOccurs="0"/>
  </xsd:sequence></xsd:complexType></xsd:element>
  <xsd:element name="title" type="xsd:string"/>
  <xsd:element name="photo" type="xsd:base64Binary"/>
  <xsd:element name="file" type="xsd:base64Binary"/>
  <xsd:element name="note" type="xsd:string"/>
</xsd:schema>`;

const TEMPLATE = `<?xml version="1.0"?>
<b:doc xmlns:b="${BL}"><b:title>T</b:title><b:photo/><b:file/><b:note/></b:doc>`;

const VIEW = `<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:b="${BL}">
  <xsl:template match="b:doc"><html><body>
    <div>Photo <span xd:xctname="InlineImage" xd:CtrlId="PHOTO" xd:binding="b:photo"/></div>
    <div>File <span xd:xctname="FileAttachment" xd:CtrlId="FILE" xd:binding="b:file"/></div>
    <div>Note <span xd:xctname="PlainText" xd:CtrlId="NOTE" xd:binding="b:note"/></div>
  </body></html></xsl:template>
</xsl:stylesheet>`;

const MANIFEST = `<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.1" productVersion="15.0.0" name="urn:example:blobs"
  xmlns:xsf="${XSF_NS}" xmlns:xsf2="${XSF2_NS}" xmlns:b="${BL}">
  <xsf:package><xsf:files>
    <xsf:file name="myschema.xsd"><xsf:fileProperties><xsf:property name="rootElement" type="string" value="doc"></xsf:property></xsf:fileProperties></xsf:file>
    <xsf:file name="template.xml"></xsf:file>
  </xsf:files></xsf:package>
  <xsf:documentSchemas><xsf:documentSchema rootSchema="yes" location="${BL} myschema.xsd"></xsf:documentSchema></xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Blobs" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:views default="Main"><xsf:view name="Main"><xsf:mainpane transform="view1.xsl"></xsf:mainpane></xsf:view></xsf:views>
</xsf:xDocumentClass>`;

export function blobXsnBytes(): Buffer {
  return buildCab([
    { name: "manifest.xsf", data: MANIFEST },
    { name: "myschema.xsd", data: SCHEMA },
    { name: "template.xml", data: TEMPLATE },
    { name: "view1.xsl", data: VIEW },
  ]);
}

export function blobFixture(): { form: FormDefinition; runtime: FormRuntime } {
  const pkg = openXsn(blobXsnBytes());
  const form = buildFormDefinition(pkg);
  return { form, runtime: new FormRuntime(createInstance(pkg, form), form) };
}
