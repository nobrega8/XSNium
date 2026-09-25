import { FormRuntime, buildFormDefinition, createInstance, openXsn, type FormDefinition } from "../../src/index.ts";
import { buildCab } from "./build-cab.ts";
import { XSF2_NS, XSF_NS } from "./manifests.ts";

export const SN = "urn:example:secondary";
export const LIST_NS = "urn:example:list";

/** A template with a dropdown whose options come from a data source named "Pilots" (a SharePoint list in real templates). */
const SCHEMA = `<xsd:schema targetNamespace="${SN}" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:s="${SN}" elementFormDefault="qualified">
  <xsd:element name="doc"><xsd:complexType><xsd:sequence><xsd:element ref="s:pick" minOccurs="0"/></xsd:sequence></xsd:complexType></xsd:element>
  <xsd:element name="pick" type="xsd:string"/>
</xsd:schema>`;

const LIST_SCHEMA = `<xsd:schema targetNamespace="${LIST_NS}" xmlns:xsd="http://www.w3.org/2001/XMLSchema" elementFormDefault="qualified">
  <xsd:element name="list"><xsd:complexType><xsd:sequence><xsd:element name="item" maxOccurs="unbounded"><xsd:complexType><xsd:sequence>
    <xsd:element name="id" type="xsd:string"/><xsd:element name="name" type="xsd:string"/>
  </xsd:sequence></xsd:complexType></xsd:element></xsd:sequence></xsd:complexType></xsd:element>
</xsd:schema>`;

const TEMPLATE = `<?xml version="1.0"?><s:doc xmlns:s="${SN}"><s:pick/></s:doc>`;

const VIEW = `<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:s="${SN}" xmlns:d="${LIST_NS}" xmlns:xdXDocument="http://schemas.microsoft.com/office/infopath/2003/xslt/xDocument">
  <xsl:template match="s:doc"><html><body>
    <div>Pilot <select xd:xctname="dropdown" xd:CtrlId="PICK" xd:binding="s:pick"><xsl:choose><xsl:when test="function-available('xdXDocument:GetDOM')"><option/>
      <xsl:for-each select="xdXDocument:GetDOM(&quot;Pilots&quot;)/d:list/d:item"><option><xsl:attribute name="value"><xsl:value-of select="d:id"/></xsl:attribute><xsl:value-of select="d:name"/></option></xsl:for-each>
    </xsl:when><xsl:otherwise><option><xsl:value-of select="s:pick"/></option></xsl:otherwise></xsl:choose></select></div>
  </body></html></xsl:template>
</xsl:stylesheet>`;

const MANIFEST = `<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.1" productVersion="15.0.0" name="urn:example:secondary"
  xmlns:xsf="${XSF_NS}" xmlns:xsf2="${XSF2_NS}" xmlns:s="${SN}">
  <xsf:package><xsf:files>
    <xsf:file name="myschema.xsd"><xsf:fileProperties><xsf:property name="rootElement" type="string" value="doc"></xsf:property></xsf:fileProperties></xsf:file>
    <xsf:file name="template.xml"></xsf:file>
  </xsf:files></xsf:package>
  <xsf:documentSchemas><xsf:documentSchema rootSchema="yes" location="${SN} myschema.xsd"></xsf:documentSchema></xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Secondary" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:views default="Main"><xsf:view name="Main"><xsf:mainpane transform="view1.xsl"></xsf:mainpane></xsf:view></xsf:views>
  <xsf:dataObjects><xsf:dataObject name="Pilots" schema="Pilots.xsd" initOnLoad="yes"><xsf:query><xsf:sharepointListAdapterRW name="Pilots" queryAllowed="yes" submitAllowed="no" siteURL="" sharePointListID=""></xsf:sharepointListAdapterRW></xsf:query></xsf:dataObject></xsf:dataObjects>
</xsf:xDocumentClass>`;

export const PILOTS_XML = `<?xml version="1.0"?><d:list xmlns:d="${LIST_NS}">
  <d:item><d:id>1</d:id><d:name>Amelia</d:name></d:item>
  <d:item><d:id>2</d:id><d:name>Bert</d:name></d:item>
  <d:item><d:id>2</d:id><d:name>Bert again</d:name></d:item>
</d:list>`;

export function secondaryXsnBytes(): Buffer {
  return buildCab([
    { name: "manifest.xsf", data: MANIFEST },
    { name: "myschema.xsd", data: SCHEMA },
    { name: "Pilots.xsd", data: LIST_SCHEMA },
    { name: "template.xml", data: TEMPLATE },
    { name: "view1.xsl", data: VIEW },
  ]);
}

export function secondaryFixture(): { form: FormDefinition; runtime: FormRuntime } {
  const pkg = openXsn(secondaryXsnBytes());
  const form = buildFormDefinition(pkg);
  return { form, runtime: new FormRuntime(createInstance(pkg, form), form) };
}
