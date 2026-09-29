/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// The web app's synthetic sample form (tests/helpers/manifests.ts and sample-form.ts), shared by the unit tests.

#pragma once

#include "testcab.hxx"

#include <string>
#include <vector>

namespace xsnium::test
{
inline const std::string MY = "urn:example:my";
inline const std::string XSF_NS = "http://schemas.microsoft.com/office/infopath/2003/solutionDefinition";
inline const std::string XSF2_NS = "http://schemas.microsoft.com/office/infopath/2006/solutionDefinition/extensions";

/** Synthetic manifest exercising the features the parser understands. */
inline const std::string SAMPLE_MANIFEST = R"(<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.7" productVersion="15.0.0"
  trustLevel="restricted" publishUrl="\\example-host\share\form.xsn" name="urn:example:form"
  xmlns:xsf=")" + XSF_NS + R"(" xmlns:xsf2=")" + XSF2_NS + R"(" xmlns:my="urn:example:my">
  <xsf:package>
    <xsf:files>
      <xsf:file name="myschema.xsd"><xsf:fileProperties>
        <xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property>
      </xsf:fileProperties></xsf:file>
      <xsf:file name="template.xml"></xsf:file>
      <xsf:file name="view1.xsl"></xsf:file>
      <xsf:file name="view2.xsl"></xsf:file>
    </xsf:files>
  </xsf:package>
  <xsf:documentSchemas>
    <xsf:documentSchema rootSchema="yes" location="urn:example:my myschema.xsd"></xsf:documentSchema>
    <xsf:documentSchema location="other.xsd"></xsf:documentSchema>
  </xsf:documentSchemas>
  <xsf:fileNew><xsf:initialXmlDocument caption="Example" href="template.xml"></xsf:initialXmlDocument></xsf:fileNew>
  <xsf:views default="Second">
    <xsf:view name="First" caption="First view">
      <xsf:mainpane transform="view1.xsl"></xsf:mainpane>
      <xsf:editing>
        <xsf:xmlToEdit name="name_1" item="/my:root/my:name"><xsf:editWith component="xField" type="plain"></xsf:editWith></xsf:xmlToEdit>
        <xsf:xmlToEdit name="late_2" item="/my:root/my:late"><xsf:editWith component="xField" type="plain"></xsf:editWith></xsf:xmlToEdit>
      </xsf:editing>
    </xsf:view>
    <xsf:view name="Second"><xsf:mainpane transform="view2.xsl"></xsf:mainpane></xsf:view>
  </xsf:views>
  <xsf:calculations>
    <xsf:calculatedField target="/my:root/my:total" expression="../my:a + ../my:b" refresh="onChange"></xsf:calculatedField>
  </xsf:calculations>
  <xsf:dataAdapters>
    <xsf:emailAdapter name="Main submit" submitAllowed="yes"><xsf:to value="someone@example.invalid"></xsf:to></xsf:emailAdapter>
    <xsf:webServiceAdapter name="Lookup" submitAllowed="no"></xsf:webServiceAdapter>
  </xsf:dataAdapters>
  <xsf:documentVersionUpgrade>
    <xsf:useTransform transform="upgrade.xsl" minVersionToUpgrade="0.0.0.0" maxVersionToUpgrade="1.0.0.6"></xsf:useTransform>
  </xsf:documentVersionUpgrade>
  <xsf:extensions><xsf:extension name="SolutionDefinitionExtensions">
    <xsf2:solutionDefinition><xsf2:managedCode language="CSharp" version="15.0"></xsf2:managedCode></xsf2:solutionDefinition>
  </xsf:extension></xsf:extensions>
</xsf:xDocumentClass>)";

inline const std::string SAMPLE_SCHEMA = R"(<xsd:schema targetNamespace="urn:example:my" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:my="urn:example:my"
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
</xsd:schema>)";

inline const std::string SAMPLE_TEMPLATE = R"(<?xml version="1.0" encoding="UTF-8"?>
<?mso-infoPathSolution name="urn:example:form" href="manifest.xsf" solutionVersion="1.0.0.7" ?>
<?mso-application progid="InfoPath.Document"?>
<my:root xmlns:my="urn:example:my" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" version="1">
	<my:title>Hello</my:title>
	<my:note xsi:nil="true"/>
	<my:items id="a"><my:name>first</my:name><my:qty>2</my:qty></my:items>
	<my:limited>x</my:limited>
</my:root>)";

/** A small but realistic view: labels, a text field, a dropdown, radios and a repeating table. */
inline const std::string SAMPLE_VIEW = R"~(<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xd="http://schemas.microsoft.com/office/infopath/2003" xmlns:my="urn:example:my">
  <xsl:template match="my:root"><html><head><style>TABLE.grid { BORDER-COLLAPSE: collapse } TD.cell { PADDING-LEFT: 7px; BEHAVIOR: url(#default#x) } .optionalPlaceholder { COLOR: #333333 }</style></head><body>
    <div>Title <span xd:xctname="PlainText" xd:CtrlId="TITLE" xd:binding="my:title"><xsl:value-of select="my:title"/></span></div>
    <div>Note <select xd:xctname="dropdown" xd:CtrlId="NOTE" xd:binding="my:note"><option>Select...</option><option value="low">Low</option><option value="high">High</option></select></div>
    <div>Late
      <input type="radio" xd:xctname="OptionButton" xd:CtrlId="R1" xd:binding="my:late" xd:onValue="yes"/> Yes
      <input type="radio" xd:xctname="OptionButton" xd:CtrlId="R2" xd:binding="my:late" xd:onValue="no"/> No
    </div>
    <xsl:choose><xsl:when test="my:late"><div>The late field exists</div></xsl:when>
      <xsl:otherwise><div class="optionalPlaceholder" xd:xmlToEdit="late_2">Add the late field</div></xsl:otherwise></xsl:choose>
    <table class="grid" style="WIDTH: 400px; TABLE-LAYOUT: fixed"><colgroup><col style="WIDTH: 400px"/></colgroup><thead><tr><td class="cell">Item name</td></tr></thead>
      <tbody xd:xctname="RepeatingTable"><xsl:for-each select="my:items"><tr><td><span xd:xctname="PlainText" xd:CtrlId="NAME" xd:binding="my:name"><xsl:value-of select="my:name"/></span></td></tr></xsl:for-each></tbody>
    </table>
  </body></html></xsl:template>
</xsl:stylesheet>)~";

inline std::string replaceAll(std::string aText, const std::string& rFrom, const std::string& rTo)
{
    for (size_t nPos = aText.find(rFrom); nPos != std::string::npos; nPos = aText.find(rFrom, nPos + rTo.size()))
        aText.replace(nPos, rFrom.size(), rTo);
    return aText;
}

/** The sample manifest with its root element declared, and `rExtra` added at the end of the document class. */
inline std::string sampleManifest(const std::string& rExtra = std::string())
{
    const std::string aNamespace = R"(<xsf:property name="namespace" type="string" value="urn:example:my"></xsf:property>)";
    return replaceAll(
        replaceAll(SAMPLE_MANIFEST, aNamespace,
                   aNamespace + R"(<xsf:property name="rootElement" type="string" value="root"></xsf:property>)"),
        "</xsf:xDocumentClass>", rExtra + "</xsf:xDocumentClass>");
}

/** The sample form as .xsn bytes, optionally with extra package files. */
inline Bytes sampleXsnBytes(const std::vector<TestFile>& rExtra = {}, const std::string& rView = SAMPLE_VIEW)
{
    std::vector<TestFile> aFiles{ { "manifest.xsf", bytes(sampleManifest()) },
                                  { "myschema.xsd", bytes(SAMPLE_SCHEMA) },
                                  { "template.xml", bytes(SAMPLE_TEMPLATE) },
                                  { "view1.xsl", bytes(rView) },
                                  { "view2.xsl", bytes(rView) } };
    aFiles.insert(aFiles.end(), rExtra.begin(), rExtra.end());
    return buildCab(aFiles);
}

/** The sample form without views, with `rManifestExtra` added to its manifest. */
inline Bytes samplePackageBytes(const std::string& rTemplate = SAMPLE_TEMPLATE, const std::string& rManifestExtra = std::string())
{
    return buildCab({ { "manifest.xsf", bytes(sampleManifest(rManifestExtra)) },
                      { "myschema.xsd", bytes(SAMPLE_SCHEMA) },
                      { "template.xml", bytes(rTemplate) } });
}
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
