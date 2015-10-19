# mod_ical

**mod_ical** is a **iCalendar/jCal/xCal filter** for [Apache httpd].
Allows iCal data to be filtered and made available to client
side web clients in iCal, xCal and jCal formats.

The module was built to allow raw iCal data to embedded within
websites and javascript applications without server side
development.


### Features

- Filter iCalendar entries to show the next entry, the last entry,
  all future entries or all past entries relative to the current
  date.

- Convert **RFC5545 iCalendar** streams into **RFC6321 xCal** XML
  streams.

- Convert **RFC5545 iCalendar** streams into **RFC7265 jCal** JSON
  streams.

- Control filtering and formatting based on optional URL query
  parameters.


### Filtering

Entries in the iCalendar stream can be filtered based on the
following options:

- **none**: No filtering, return all entries.

- **next**: Return the next entry relative to the current date. Can
  be used to indicate upcoming events in a web application.

- **last**: Return the last entry relative to the current date. Can
  be used to highlight the most recently passed event in a web
  application.

- **future**: Return all entries whose end is past the current date.
  Can be used to list all upcoming events.

- **past**: Return all entries whose end is in the past relative to
  the current date. Can be used to list all past events.


### Conversion

The output format of the iCalendar stream can be controlled
through the use of the following filters:

- **ICAL**: Based on the contents of the Accept header, return the
  RFC5545 iCal streams, RFC6321 xCal streams or RFC7265 jCal
  streams depending on the client request. Can be used where
  control over the desired output format is requested by a
  client application.

- **ICALICAL**: Force output to RFC5545 iCal.

- **ICALXCAL**: Force output to RFC6321 xCal. Can be used where the
  output is being processed on the server side by XSLT.

- **ICALJCAL**: Force output to RFC7265 jCal. Can be used where
  the client is not able to control the Accept header.


### Configuration Directives

- **ICalFilter**: Set the filtering to 'none', 'next', 'last', future'
  or 'past'. Defaults to 'past'.

- **ICalFormat**: Set the formatting to 'none', 'spaced' or 'pretty'.
  Defaults to 'none'.


### Query Parameters

The configuration directives above can be overridden on a per URL
basis by the addition of the following optional query parameters:

- **filter**: Set the filtering to 'none', 'next', 'last', future'
  or 'past'.

- **format**: Set the formatting to 'none', 'spaced' or 'pretty'.

```
http://example.com/calendars/upcoming-events.ics?filter=next&format=pretty
```


### Example Configuration

The following example configuration shows how the **ICAL** filter
can be added to the [Apache httpd] configuration.

```
LoadModule ical_module modules/mod_ical.so

<Location /calendars/>
  SetOutputFilter ICAL
  ICalFilter none
  ICalFormat pretty
</Location>
```

Using an additional PATH_INFO at the end of the calendar URL, explicit
filters can be assigned to each URL as follows:

```
AcceptPathInfo on

# create a dedicated JSON view
# http://example.com/breaking-news.ics/json
<If "%{PATH_INFO} =~ /json/">
  SetOutputFilter ICALJCAL
</If>
# create a news view, passed through XSLT
# http://example.com/breaking-news.ics/news
<If "%{PATH_INFO} =~ /news/">
  SetOutputFilter ICALXCAL;XSLT
  TransformOptions +ApacheFS +XIncludes
  TransformSet /news.xsl
</If>
```

The following is an example of an XSLT stylesheet that could be used in
the example above:

```
<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:xcal="urn:ietf:params:xml:ns:icalendar-2.0" xmlns:date="http://exslt.org/dates-and-times" xmlns="http://www.w3.org/1999/xhtml" version="1.
0">
<xsl:output omit-xml-declaration="yes" method="html"/>
<xsl:template match="/xcal:icalendar">
  <xsl:for-each select="xcal:vcalendar/xcal:components/xcal:vevent">
    <xsl:choose>
      <xsl:when test="xcal:properties/xcal:url">
        <h3 class="vevent"><strong><xsl:text>Join us for </xsl:text><a class="summary"><xsl:attribute name="href"><xsl:value-of select="xcal:properties/xcal:url/xcal:uri"/></xsl:attribute><xsl:value-of select="xcal:properties/xcal:summary/xcal:text"/></a></strong><xsl:text> on </xsl:text><span class="dtstart"><xsl:attribute name="title"><xsl:value-of select="xcal:properties/xcal:dtstart/xcal:date-time"/></xsl:attribute><xsl:value-of select="date:day-in-month(xcal:properties/xcal:dtstart/xcal:date-time)"/><xsl:text> </xsl:text><xsl:value-of select="date:month-name(xcal:properties/xcal:dtstart/xcal:date-time)"/><xsl:text> at </xsl:text><xsl:value-of select="substring(date:time(xcal:properties/xcal:dtstart/xcal:date-time),0,6)"/></span><xsl:text> </xsl:text><span class="description"><xsl:value-of select="xcal:properties/xcal:description/xcal:text"/></span></h3>
      </xsl:when>
      <xsl:otherwise>
        <h3 class="vevent"><strong><xsl:text>Join us for </xsl:text><span class="summary"><xsl:value-of select="xcal:properties/xcal:summary/xcal:text"/></span></strong><xsl:text> on </xsl:text><span class="dtstart"><xsl:attribute name="title"><xsl:value-of select="xcal:properties/xcal:dtstart/xcal:date-time"/></xsl:attribute><xsl:value-of select="date:day-in-month(xcal:properties/xcal:dtstart/xcal:date-time)"/><xsl:text> </xsl:text><xsl:value-of select="date:month-name(xcal:properties/xcal:dtstart/xcal:date-time)"/><xsl:text> at </xsl:text><xsl:value-of select="substring(date:time(xcal:properties/xcal:dtstart/xcal:date-time),0,6)"/></span><xsl:text> </xsl:text><span class="description"><xsl:value-of select="xcal:properties/xcal:description/xcal:text"/></span></h3>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:for-each>

</xsl:template>
</xsl:stylesheet>
```

For more advanced configurations, see the **mod_filter** documentation for
[Apache httpd].


### Version

0.0.2: Bugfix release.


### Bugs

mod_ical has received light testing with respect to RFC compliance,
and RFC compliance violations will be considered bugs and fixed in
future versions.

### License

mod_ical is released under the Apache License v2.


### Dependencies

mod_ical depends on:

* [Apache httpd] - Apache httpd web server
* [libical] - Libical iCalendar library
* [libxml2] - libxml2 XML library
* [json-c] - json-c JSON library


  [Apache httpd]: <http://httpd.apache.org>
  [libical]: <https://github.com/libical/libical>
  [libxml2]: <http://www.xmlsoft.org/>
  [json-c]: <https://github.com/json-c/json-c/wiki>
