export const XSF_NS = "http://schemas.microsoft.com/office/infopath/2003/solutionDefinition";
export const XSF2_NS = "http://schemas.microsoft.com/office/infopath/2006/solutionDefinition/extensions";

/** Synthetic manifest exercising the features the parser understands. */
export const SAMPLE_MANIFEST = `<?xml version="1.0" encoding="UTF-8"?>
<xsf:xDocumentClass solutionFormatVersion="15.0.0.0" solutionVersion="1.0.0.7" productVersion="15.0.0"
  trustLevel="restricted" publishUrl="\\\\example-host\\share\\form.xsn" name="urn:example:form"
  xmlns:xsf="${XSF_NS}" xmlns:xsf2="${XSF2_NS}" xmlns:my="urn:example:my">
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
</xsf:xDocumentClass>`;
