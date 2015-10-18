/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * mod_ical.c: iCalendar filters
 *
 * ical2xcal: Convert an iCalendar stream into an XML stream (rfc6321)
 *
 * icalfilter: Show next / last calendar item based on today's date.
 *
 * Convert iCalendar into the local timezone of the client?
 */

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "util_filter.h"
#include "apr_strings.h"
#include "apr_lib.h"

#include <libical/ical.h>
#include <libical/icalclassify.h>
#include <libical/icalrecur.h>

#include <libxml/encoding.h>
#include <libxml/xmlwriter.h>

#include <json-c/json.h>

#include <string.h>

module AP_MODULE_DECLARE_DATA ical_module;


#define DEFAULT_ICAL_FILTER AP_ICAL_FILTER_NEXT
#define DEFAULT_ICAL_FORMAT AP_ICAL_FORMAT_NONE

#define XCAL_HEADER "<?xml version=\"1.0\" encoding=\"utf-8\"?>" \
  "<icalendar xmlns=\"urn:ietf:params:xml:ns:icalendar-2.0\">"
#define XCAL_FOOTER "</icalendar>"

typedef enum {
    AP_ICAL_FILTER_NONE,
    AP_ICAL_FILTER_NEXT,
    AP_ICAL_FILTER_LAST,
    AP_ICAL_FILTER_FUTURE,
    AP_ICAL_FILTER_PAST,
    AP_ICAL_FILTER_UNKNOWN
} ap_ical_filter_e;

typedef enum {
    AP_ICAL_FORMAT_NONE,
    AP_ICAL_FORMAT_SPACED,
    AP_ICAL_FORMAT_PRETTY,
    AP_ICAL_FORMAT_UNKNOWN
} ap_ical_format_e;

typedef enum {
    AP_ICAL_OUTPUT_NEGOTIATED,
    AP_ICAL_OUTPUT_ICAL,
    AP_ICAL_OUTPUT_XCAL,
    AP_ICAL_OUTPUT_JCAL
} ap_ical_output_e;

typedef struct ical_ctx {
    apr_bucket_brigade *bb;
    apr_bucket_brigade *tmp;
    icalparser *parser;
    int seen_eol;
    int eat_crlf;
    int seen_eos;
    ap_ical_output_e output;
    ap_ical_filter_e filter;
    ap_ical_format_e format;
} ical_ctx;

typedef struct ical_conf {
    unsigned int filter_set:1; /* has filtering been set */
    unsigned int format_set:1; /* has formatting been set */
    ap_ical_filter_e filter; /* type of filtering */
    ap_ical_format_e format; /* type of formatting */
} ical_conf;

static apr_status_t icalparser_cleanup(void *data)
{
    icalparser *parser = data;
    icalparser_free(parser);
    return APR_SUCCESS;
}

static apr_status_t icalcomponent_cleanup(void *data)
{
    icalcomponent *comp = data;
    icalcomponent_free(comp);
    return APR_SUCCESS;
}

static apr_status_t jsonbuffer_cleanup(void *data)
{
    json_object *buf = data;
    json_object_put(buf);
    return APR_SUCCESS;
}

static apr_status_t xmlbuffer_cleanup(void *data)
{
    xmlBufferPtr buf = data;
    xmlBufferFree(buf);
    return APR_SUCCESS;
}

static apr_status_t xmlwriter_cleanup(void *data)
{
    xmlTextWriterPtr writer = data;
    xmlFreeTextWriter(writer);
    return APR_SUCCESS;
}

static char *strlwr(char *str)
{
    apr_size_t i;
    apr_size_t len = strlen(str);

    for (i = 0; i < len; i++)
        str[i] = apr_tolower((unsigned char) str[i]);

    return str;
}

static const char *icalrecur_weekday_to_string(icalrecurrencetype_weekday kind)
{
    switch (kind) {
    case ICAL_SUNDAY_WEEKDAY: {
        return "SU";
    }
    case ICAL_MONDAY_WEEKDAY: {
        return "MO";
    }
    case ICAL_TUESDAY_WEEKDAY: {
        return "TU";
    }
    case ICAL_WEDNESDAY_WEEKDAY: {
        return "WE";
    }
    case ICAL_THURSDAY_WEEKDAY: {
        return "TH";
    }
    case ICAL_FRIDAY_WEEKDAY: {
        return "FR";
    }
    case ICAL_SATURDAY_WEEKDAY: {
        return "SA";
    }
    default: {
        return "UNKNOWN";
    }
    }
}

#define ICAL_LEAP_MONTH 0x1000

static int icalrecurrencetype_month_is_leap(short month)
{
    return (month & ICAL_LEAP_MONTH);
}

static int icalrecurrencetype_month_month(short month)
{
    return (month & ~ICAL_LEAP_MONTH);
}

static apr_status_t icalduration_to_json(const char *element,
        struct icaldurationtype duration, json_object *jarray)
{
    apr_status_t rv = APR_SUCCESS;

    /* write duration element */
    {
        char *str = icaldurationtype_as_ical_string_r(duration);
        json_object_array_add(jarray, json_object_new_string(str));
        icalmemory_free_buffer(str);
    }

    return rv;
}

static apr_status_t icalduration_to_xml(const char *element,
        struct icaldurationtype duration, xmlTextWriterPtr writer)
{
    apr_status_t rv = APR_SUCCESS;
    int rc;

    /* write duration element */
    {
        char *str = icaldurationtype_as_ical_string_r(duration);
        rc = xmlTextWriterWriteFormatElement(writer,
                BAD_CAST element, "%s", str);
        icalmemory_free_buffer(str);
    }

    return rv;
}

static apr_status_t icaltime_to_json(ap_filter_t *f, const char *element,
        struct icaltimetype tt, json_object *jarray)
{
    apr_status_t rv = APR_SUCCESS;

    if (tt.is_date) {
        json_object_array_add(jarray,
                json_object_new_string(
                        apr_psprintf(f->r->pool, "%04d-%02d-%02d", tt.year,
                                tt.month, tt.day)));
    }
    else {
        json_object_array_add(jarray,
                json_object_new_string(
                        apr_psprintf(f->r->pool,
                                "%04d-%02d-%02dT%02d:%02d:%02d", tt.year,
                                tt.month, tt.day, tt.hour, tt.minute,
                                tt.second)));
    }

    return rv;
}

static apr_status_t icaltime_to_xml(const char *element, struct icaltimetype tt,
        xmlTextWriterPtr writer)
{
    apr_status_t rv = APR_SUCCESS;
    int rc;

    if (tt.is_date) {
        rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST element, "%04d-%02d-%02d", tt.year,
                tt.month, tt.day);
        if (rc < 0) {
            return APR_EGENERAL;
        }
    }
    else {
        rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST element,
                "%04d-%02d-%02dT%02d:%02d:%02d", tt.year, tt.month, tt.day,
                tt.hour, tt.minute, tt.second);
        if (rc < 0) {
            return APR_EGENERAL;
        }
    }

    return rv;
}

static apr_status_t icalrecurrence_byday_to_json(ap_filter_t *f,
        const char *element, short *array, short limit, json_object *jobj)
{
    int i;

    if (array[0] != ICAL_RECURRENCE_ARRAY_MAX) {

        json_object *jarray = json_object_new_array();
        json_object_object_add(jobj, element, jarray);

        for (i = 0; i < limit && array[i] != ICAL_RECURRENCE_ARRAY_MAX; i++) {

            int pos = icalrecurrencetype_day_position(array[i]);
            int dow = icalrecurrencetype_day_day_of_week(array[i]);
            const char *daystr = icalrecur_weekday_to_string(dow);

            if (pos == 0) {

                json_object_array_add(jarray,
                        json_object_new_string(
                                apr_psprintf(f->r->pool, "%s", daystr)));

            }
            else {

                json_object_array_add(jarray,
                        json_object_new_string(
                                apr_psprintf(f->r->pool, "%d%s", pos, daystr)));

            }

        }

    }

    return APR_SUCCESS;
}

static apr_status_t icalrecurrence_byday_to_xml(const char *element,
        short *array, short limit, xmlTextWriterPtr writer)
{
    int i, rc;

    for (i = 0; i < limit && array[i] != ICAL_RECURRENCE_ARRAY_MAX; i++) {

        int pos = icalrecurrencetype_day_position(array[i]);
        int dow = icalrecurrencetype_day_day_of_week(array[i]);
        const char *daystr = icalrecur_weekday_to_string(dow);

        if (pos == 0) {
            rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST element,
                    "%s", daystr);
        }
        else {
            rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST element,
                    "%d%s", pos, daystr);
        }
        if (rc < 0) {
            return APR_EGENERAL;
        }

    }

    return APR_SUCCESS;
}

static apr_status_t icalrecurrence_bymonth_to_json(ap_filter_t *f,
        const char *element, short *array, short limit, json_object *jobj)
{
    int i;

    if (array[0] != ICAL_RECURRENCE_ARRAY_MAX) {

        json_object *jarray = json_object_new_array();
        json_object_object_add(jobj, element, jarray);

        for (i = 0; i < limit && array[i] != ICAL_RECURRENCE_ARRAY_MAX; i++) {

            /* rfc7529 introduces the leap month */
            if (icalrecurrencetype_month_is_leap(array[i])) {

                json_object_array_add(jarray,
                        json_object_new_string(
                                apr_psprintf(f->r->pool, "%dL",
                                        icalrecurrencetype_month_month(
                                                array[i]))));

            }
            else {

                json_object_array_add(jarray,
                        json_object_new_int(
                                (int32_t) icalrecurrencetype_month_month(
                                        array[i])));

            }

        }

    }

    return APR_SUCCESS;
}

static apr_status_t icalrecurrence_bymonth_to_xml(const char *element,
        short *array, short limit, xmlTextWriterPtr writer)
{
    int i, rc;

    for (i = 0; i < limit && array[i] != ICAL_RECURRENCE_ARRAY_MAX; i++) {

        if (icalrecurrencetype_month_is_leap(array[i])) {
            rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST element,
                    "%dL", icalrecurrencetype_month_month(array[i]));
        }
        else {
            rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST element,
                    "%d", array[i]);
        }
        if (rc < 0) {
            return APR_EGENERAL;
        }

    }

    return APR_SUCCESS;
}

static apr_status_t icalrecurrence_by_to_json(const char *element, short *array,
        short limit, json_object *jobj)
{
    int i;

    if (array[0] != ICAL_RECURRENCE_ARRAY_MAX) {

        json_object *jarray = json_object_new_array();
        json_object_object_add(jobj, element, jarray);

        for (i = 0; i < limit && array[i] != ICAL_RECURRENCE_ARRAY_MAX; i++) {

            json_object_array_add(jarray,
                    json_object_new_int((int32_t) array[i]));

        }

    }

    return APR_SUCCESS;
}

static apr_status_t icalrecurrence_by_to_xml(const char *element, short *array,
        short limit, xmlTextWriterPtr writer)
{
    int i, rc;

    for (i = 0; i < limit && array[i] != ICAL_RECURRENCE_ARRAY_MAX; i++) {

        rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST element,
                "%d", array[i]);
        if (rc < 0) {
            return APR_EGENERAL;
        }

    }

    return APR_SUCCESS;
}

static apr_status_t icalrecurrencetype_to_json(ap_filter_t *f,
        struct icalrecurrencetype *recur, json_object *jarray)
{
    apr_status_t rv = APR_SUCCESS;
    int rc;

    json_object *jobj = json_object_new_object();
    json_object_array_add(jarray, jobj);

    if (recur->freq != ICAL_NO_RECURRENCE) {

        if (recur->until.year != 0) {

            if (recur->until.is_date) {
                json_object_object_add(jobj, "until",
                        json_object_new_string(
                                apr_psprintf(f->r->pool, "%04d-%02d-%02d",
                                        recur->until.year, recur->until.month,
                                        recur->until.day)));
            }
            else {
                json_object_object_add(jobj, "until",
                        json_object_new_string(
                                apr_psprintf(f->r->pool,
                                        "%04d-%02d-%02dT%02d:%02d:%02d",
                                        recur->until.year, recur->until.month,
                                        recur->until.day, recur->until.hour,
                                        recur->until.minute,
                                        recur->until.second)));
            }

        }

        if (recur->count != 0) {

            json_object_object_add(jobj, "count",
                    json_object_new_int((int32_t) recur->count));

        }

        if (recur->interval != 1) {

            json_object_object_add(jobj, "interval",
                    json_object_new_int((int32_t) recur->interval));

        }

        rc = icalrecurrence_by_to_json("bysecond", recur->by_second,
                ICAL_BY_SECOND_SIZE, jobj);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_json("byminute", recur->by_minute,
                ICAL_BY_MINUTE_SIZE, jobj);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_json("byhour", recur->by_hour,
                ICAL_BY_HOUR_SIZE, jobj);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_byday_to_json(f, "byday", recur->by_day, ICAL_BY_DAY_SIZE,
                jobj);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_json("bymonthday", recur->by_month_day,
                ICAL_BY_MONTHDAY_SIZE, jobj);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_json("byyearday", recur->by_year_day,
                ICAL_BY_YEARDAY_SIZE, jobj);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_json("byweekno", recur->by_week_no,
                ICAL_BY_WEEKNO_SIZE, jobj);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_bymonth_to_json(f, "bymonth", recur->by_month,
                ICAL_BY_MONTH_SIZE, jobj);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_json("bysetpos", recur->by_set_pos,
                ICAL_BY_SETPOS_SIZE, jobj);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        /* Monday is the default, so no need to write that out */
        if (recur->week_start != ICAL_MONDAY_WEEKDAY
                && recur->week_start != ICAL_NO_WEEKDAY) {

            int dow = icalrecurrencetype_day_day_of_week(recur->week_start);

            json_object_object_add(jobj, "wkst",
                    json_object_new_string(
                            apr_psprintf(f->r->pool, "%s",
                                    icalrecur_weekday_to_string(dow))));

        }

    }

    return rv;
}

static apr_status_t icalrecurrencetype_to_xml(ap_filter_t *f,
        struct icalrecurrencetype *recur, xmlTextWriterPtr writer)
{
    apr_status_t rv = APR_SUCCESS;
    int rc;

    if (recur->freq != ICAL_NO_RECURRENCE) {

        if (recur->until.year != 0) {

            if (recur->until.is_date) {
                rc = xmlTextWriterWriteFormatElement(writer,
                        BAD_CAST "until",
                        "%04d-%02d-%02d", recur->until.year,
                        recur->until.month, recur->until.day);
            }
            else {
                rc = xmlTextWriterWriteFormatElement(writer,
                        BAD_CAST "until",
                        "%04d-%02d-%02dT%02d:%02d:%02d",
                        recur->until.year, recur->until.month,
                        recur->until.day, recur->until.hour,
                        recur->until.minute, recur->until.second);
            }

        }

        if (recur->count != 0) {

            rc = xmlTextWriterWriteFormatElement(writer,
                    BAD_CAST "count", "%d", recur->count);
            if (rc < 0) {
                return APR_EGENERAL;
            }

        }

        if (recur->interval != 1) {

            rc = xmlTextWriterWriteFormatElement(writer,
                    BAD_CAST "interval", "%d", recur->interval);
            if (rc < 0) {
                return APR_EGENERAL;
            }

        }

        rc = icalrecurrence_by_to_xml("bysecond", recur->by_second,
                ICAL_BY_SECOND_SIZE, writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_xml("byminute", recur->by_minute,
                ICAL_BY_MINUTE_SIZE, writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_xml("byhour", recur->by_hour,
                ICAL_BY_HOUR_SIZE, writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_byday_to_xml("byday", recur->by_day, ICAL_BY_DAY_SIZE,
                writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_xml("bymonthday", recur->by_month_day,
                ICAL_BY_MONTHDAY_SIZE, writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_xml("byyearday", recur->by_year_day,
                ICAL_BY_YEARDAY_SIZE, writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_xml("byweekno", recur->by_week_no,
                ICAL_BY_WEEKNO_SIZE, writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_bymonth_to_xml("bymonth", recur->by_month,
                ICAL_BY_MONTH_SIZE, writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        rc = icalrecurrence_by_to_xml("bysetpos", recur->by_set_pos,
                ICAL_BY_SETPOS_SIZE, writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        /* Monday is the default, so no need to write that out */
        if (recur->week_start != ICAL_MONDAY_WEEKDAY &&
            recur->week_start != ICAL_NO_WEEKDAY) {

            int dow = icalrecurrencetype_day_day_of_week(
                    recur->week_start);

            rc = xmlTextWriterWriteFormatElement(writer,
                    BAD_CAST "wkst", "%s",
                    icalrecur_weekday_to_string(dow));
            if (rc < 0) {
                return APR_EGENERAL;
            }

        }

    }

    return rv;
}

static apr_status_t icalvalue_multi_to_json(ap_filter_t *f, icalvalue *val,
        json_object *jarray)
{
    if (val) {
        char *str = icalvalue_as_ical_string_r(val);

        if (str) {
            const char *slider = str;

            while (slider) {
                const char *token = slider;
                slider = ap_strchr(slider, ',');

                if (slider) {
                    json_object_array_add(jarray,
                            json_object_new_string(
                                    apr_psprintf(f->r->pool, "%.*s",
                                            (int) (slider - token), token)));
                    slider++;
                }
                else {
                    json_object_array_add(jarray,
                            json_object_new_string(token));
                }

            }

            icalmemory_free_buffer(str);
        }

    }

    return APR_SUCCESS;
}

static apr_status_t icalvalue_multi_to_xml(ap_filter_t *f,
        icalvalue *val, xmlTextWriterPtr writer)
{
    int rc = 0;

    if (val) {
        char *str;
        icalvalue_kind kind = icalvalue_isa(val);
        char *element = NULL;

        /* work out the value type */
        if (kind != ICAL_X_VALUE) {
            element = apr_pstrdup(f->r->pool, icalvalue_kind_to_string(kind));
        }
        if (element) {
            element = strlwr(element);
        }
        else {
            element = "unknown";
        }

        /* write out each value */
        str = icalvalue_as_ical_string_r(val);
        if (str) {
            const char *slider = str;

            while (slider) {
                const char *token = slider;
                slider = ap_strchr(slider, ',');

                if (slider) {
                    rc = xmlTextWriterWriteFormatElement(writer,
                            BAD_CAST element, "%.*s",
                            (int) (slider - token), token);
                    slider++;
                }
                else {
                    rc = xmlTextWriterWriteFormatElement(writer,
                            BAD_CAST element, "%s", token);
                }

            }

            icalmemory_free_buffer(str);
        }

        if (rc < 0) {
            return APR_EGENERAL;
        }

    }

    return APR_SUCCESS;
}

static apr_status_t icalvalue_to_json(ap_filter_t *f, icalvalue *val,
        json_object *jarray)
{
    apr_status_t rv = APR_SUCCESS;

    if (val) {

        icalvalue_kind kind = icalvalue_isa(val);
        char *element = NULL;

        /* work out the value type */
        if (kind != ICAL_X_VALUE) {
            element = apr_pstrdup(f->r->pool, icalvalue_kind_to_string(kind));
        }
        if (element) {
            element = strlwr(element);
        }
        else {
            element = "unknown";
        }

        /* handle each type */
        switch (kind) {
        case ICAL_ACTION_VALUE:
        case ICAL_ATTACH_VALUE:
        case ICAL_BINARY_VALUE:
        case ICAL_BOOLEAN_VALUE:
        case ICAL_CALADDRESS_VALUE:
        case ICAL_CARLEVEL_VALUE:
        case ICAL_CLASS_VALUE:
        case ICAL_CMD_VALUE:
        case ICAL_FLOAT_VALUE:
        case ICAL_INTEGER_VALUE:
        case ICAL_METHOD_VALUE:
        case ICAL_QUERY_VALUE:
        case ICAL_QUERYLEVEL_VALUE:
        case ICAL_STATUS_VALUE:
        case ICAL_STRING_VALUE:
        case ICAL_TRANSP_VALUE:
        case ICAL_URI_VALUE:
        case ICAL_UTCOFFSET_VALUE:
        {
            char *str = icalvalue_as_ical_string_r(val);
            json_object_array_add(jarray, json_object_new_string(element));
            json_object_array_add(jarray, json_object_new_string(str));
            icalmemory_free_buffer(str);

            break;
        }
        case ICAL_GEO_VALUE: {
            struct icalgeotype geo = icalvalue_get_geo(val);
            json_object *jvalue = json_object_new_array();

            json_object_array_add(jarray, json_object_new_string("float"));

            json_object_array_add(jvalue, json_object_new_double(geo.lat));
            json_object_array_add(jvalue, json_object_new_double(geo.lon));

            json_object_array_add(jarray, jvalue);

            break;
        }
        case ICAL_TEXT_VALUE: {
            /* we explicitly don't escape text here */
            const char* text = icalvalue_get_text(val);
            json_object_array_add(jarray, json_object_new_string(element));
            json_object_array_add(jarray, json_object_new_string(text));

            break;
        }
        case ICAL_REQUESTSTATUS_VALUE: {
            struct icalreqstattype requeststatus = icalvalue_get_requeststatus(val);
            json_object *jvalue = json_object_new_array();

            json_object_array_add(jarray, json_object_new_string("text"));

            json_object_array_add(jvalue, json_object_new_string(icalenum_reqstat_code(requeststatus.code)));
            json_object_array_add(jvalue, json_object_new_string(requeststatus.desc));

            if (requeststatus.debug) {
                json_object_array_add(jvalue, json_object_new_string(requeststatus.debug));
            }

            json_object_array_add(jarray, jvalue);

            break;
        }
        case ICAL_PERIOD_VALUE: {
            struct icalperiodtype period = icalvalue_get_period(val);
            json_object *jvalue = json_object_new_array();

            json_object_array_add(jarray, json_object_new_string(element));

            rv = icaltime_to_json(f, "start", period.start, jvalue);
            if (rv != APR_SUCCESS) {
                return rv;
            }

            if (!icaltime_is_null_time(period.end)) {
                icaltime_to_json(f, "end", period.start, jvalue);
            }
            else {
                icalduration_to_json("duration", period.duration, jvalue);
            }

            json_object_array_add(jarray, jvalue);

            break;
        }
        case ICAL_DATETIMEPERIOD_VALUE: {
            struct icaldatetimeperiodtype datetimeperiod =
                    icalvalue_get_datetimeperiod(val);
            json_object *jvalue = json_object_new_array();

            json_object_array_add(jarray, json_object_new_string(element));

            if (!icaltime_is_null_time(datetimeperiod.time)) {
                icaltime_to_json(f, "time", datetimeperiod.time, jvalue);
            }
            else {
                rv = icaltime_to_json(f, "start", datetimeperiod.period.start,
                        jvalue);
                if (rv != APR_SUCCESS) {
                    return rv;
                }

                if (!icaltime_is_null_time(datetimeperiod.period.end)) {
                    icaltime_to_json(f, "end", datetimeperiod.period.start, jvalue);
                }
                else {
                    icalduration_to_json("duration",
                            datetimeperiod.period.duration, jvalue);
                }
            }

            json_object_array_add(jarray, jvalue);

            break;
        }
        case ICAL_DURATION_VALUE: {
            struct icaldurationtype duration = icalvalue_get_duration(val);

            json_object_array_add(jarray, json_object_new_string(element));
            json_object_array_add(jarray,
                    json_object_new_string(
                            icaldurationtype_as_ical_string(duration)));

            break;
        }
        case ICAL_X_VALUE: {
            const char* x = icalvalue_get_x(val);

            json_object_array_add(jarray, json_object_new_string(element));
            json_object_array_add(jarray, json_object_new_string(x));

            break;
        }
        case ICAL_RECUR_VALUE: {
            struct icalrecurrencetype recur = icalvalue_get_recur(val);

            json_object_array_add(jarray, json_object_new_string(element));
            rv = icalrecurrencetype_to_json(f, &recur, jarray);
            if (rv != APR_SUCCESS) {
                return rv;
            }

            break;
        }
        case ICAL_TRIGGER_VALUE: {
            struct icaltriggertype trigger = icalvalue_get_trigger(val);

            json_object_array_add(jarray, json_object_new_string(element));
            if (!icaltime_is_null_time(trigger.time)) {
                icaltime_to_json(f, "time", trigger.time, jarray);
            }
            else {
                icalduration_to_json("duration", trigger.duration, jarray);
            }

            break;
        }
        case ICAL_DATE_VALUE: {
            struct icaltimetype date = icalvalue_get_date(val);

            json_object_array_add(jarray, json_object_new_string(element));
            json_object_array_add(jarray,
                    json_object_new_string(
                            apr_psprintf(f->r->pool, "%04d-%02d-%02d",
                                    date.year, date.month, date.day)));

            break;
        }
        case ICAL_DATETIME_VALUE: {
            struct icaltimetype datetime = icalvalue_get_datetime(val);

            json_object_array_add(jarray, json_object_new_string(element));
            json_object_array_add(jarray,
                    json_object_new_string(
                            apr_psprintf(f->r->pool,
                                    "%04d-%02d-%02dT%02d:%02d:%02d",
                                    datetime.year, datetime.month, datetime.day,
                                    datetime.hour, datetime.minute,
                                    datetime.second)));

            break;
        }
        default: {
            /* if we don't recognise it, add it as a string */
            char *str = icalvalue_as_ical_string_r(val);
            json_object_array_add(jarray, json_object_new_string(element));
            json_object_array_add(jarray, json_object_new_string(str));
            icalmemory_free_buffer(str);

            break;
        }
        }

    }

    return rv;
}

static apr_status_t icalvalue_to_xml(ap_filter_t *f, icalvalue *val,
        xmlTextWriterPtr writer)
{
    apr_status_t rv = APR_SUCCESS;
    int rc = 0;

    if (val) {
        icalvalue_kind kind = icalvalue_isa(val);
        char *element = NULL;

        /* work out the value type */
        if (kind != ICAL_X_VALUE) {
            element = apr_pstrdup(f->r->pool, icalvalue_kind_to_string(kind));
        }
        if (element) {
            element = strlwr(element);
        }
        else {
            element = "unknown";
        }

        /* open value element */
        rc = xmlTextWriterStartElement(writer, BAD_CAST
                element);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        /* handle each type */
        switch (kind) {
        case ICAL_ACTION_VALUE:
        case ICAL_ATTACH_VALUE:
        case ICAL_BINARY_VALUE:
        case ICAL_BOOLEAN_VALUE:
        case ICAL_CALADDRESS_VALUE:
        case ICAL_CARLEVEL_VALUE:
        case ICAL_CLASS_VALUE:
        case ICAL_CMD_VALUE:
        case ICAL_FLOAT_VALUE:
        case ICAL_INTEGER_VALUE:
        case ICAL_METHOD_VALUE:
        case ICAL_QUERY_VALUE:
        case ICAL_QUERYLEVEL_VALUE:
        case ICAL_STATUS_VALUE:
        case ICAL_STRING_VALUE:
        case ICAL_TRANSP_VALUE:
        case ICAL_URI_VALUE:
        case ICAL_UTCOFFSET_VALUE:
        {
            char *str = icalvalue_as_ical_string_r(val);
            rc = xmlTextWriterWriteFormatString(writer, "%s", str);
            icalmemory_free_buffer(str);

            break;
        }
        case ICAL_GEO_VALUE: {
            struct icalgeotype geo = icalvalue_get_geo(val);

            rc = xmlTextWriterStartElement(writer, BAD_CAST "latitude");
            if (rc < 0) {
                return APR_EGENERAL;
            }

            rc = xmlTextWriterWriteFormatString(writer, "%f", geo.lat);
            if (rc < 0) {
                return APR_EGENERAL;
            }

            rc = xmlTextWriterEndElement(writer);
            if (rc < 0) {
                return APR_EGENERAL;
            }

            rc = xmlTextWriterStartElement(writer, BAD_CAST "longitude");
            if (rc < 0) {
                return APR_EGENERAL;
            }

            rc = xmlTextWriterWriteFormatString(writer, "%f", geo.lon);
            if (rc < 0) {
                return APR_EGENERAL;
            }

            rc = xmlTextWriterEndElement(writer);
            if (rc < 0) {
                return APR_EGENERAL;
            }

            break;
        }
        case ICAL_TEXT_VALUE: {
            /* we explicitly don't escape text here */
            const char* text = icalvalue_get_text(val);
            rc = xmlTextWriterWriteFormatString(writer, "%s", text);

            break;
        }
        case ICAL_REQUESTSTATUS_VALUE: {
            struct icalreqstattype requeststatus = icalvalue_get_requeststatus(val);

            rc = xmlTextWriterStartElement(writer, BAD_CAST "code");
            if (rc < 0) {
                return APR_EGENERAL;
            }

            rc = xmlTextWriterWriteFormatString(writer, "%s",
                    icalenum_reqstat_code(requeststatus.code));
            if (rc < 0) {
                return APR_EGENERAL;
            }

            rc = xmlTextWriterEndElement(writer);
            if (rc < 0) {
                return APR_EGENERAL;
            }

            rc = xmlTextWriterStartElement(writer, BAD_CAST "description");
            if (rc < 0) {
                return APR_EGENERAL;
            }

            rc = xmlTextWriterWriteFormatString(writer, "%s", requeststatus.desc);
            if (rc < 0) {
                return APR_EGENERAL;
            }

            rc = xmlTextWriterEndElement(writer);
            if (rc < 0) {
                return APR_EGENERAL;
            }

            if (requeststatus.debug) {

                rc = xmlTextWriterStartElement(writer, BAD_CAST "data");
                if (rc < 0) {
                    return APR_EGENERAL;
                }

                rc = xmlTextWriterWriteFormatString(writer, "%s",
                        requeststatus.debug);
                if (rc < 0) {
                    return APR_EGENERAL;
                }

                rc = xmlTextWriterEndElement(writer);
                if (rc < 0) {
                    return APR_EGENERAL;
                }

            }

            break;
        }
        case ICAL_PERIOD_VALUE: {
            struct icalperiodtype period = icalvalue_get_period(val);

            rv = icaltime_to_xml("start", period.start, writer);
            if (rv != APR_SUCCESS) {
                return rv;
            }

            if (!icaltime_is_null_time(period.end)) {
                icaltime_to_xml("end", period.start, writer);
            }
            else {
                icalduration_to_xml("duration", period.duration, writer);
            }

            break;
        }
        case ICAL_DATETIMEPERIOD_VALUE: {
            struct icaldatetimeperiodtype datetimeperiod =
                    icalvalue_get_datetimeperiod(val);

            if (!icaltime_is_null_time(datetimeperiod.time)) {
                icaltime_to_xml("time", datetimeperiod.time, writer);
            }
            else {
                rv = icaltime_to_xml("start", datetimeperiod.period.start,
                        writer);
                if (rv != APR_SUCCESS) {
                    return rv;
                }

                if (!icaltime_is_null_time(datetimeperiod.period.end)) {
                    icaltime_to_xml("end", datetimeperiod.period.start, writer);
                }
                else {
                    icalduration_to_xml("duration",
                            datetimeperiod.period.duration, writer);
                }
            }

            break;
        }
        case ICAL_DURATION_VALUE: {
            struct icaldurationtype duration = icalvalue_get_duration(val);
            rc = xmlTextWriterWriteFormatString(writer, "%s",
                    icaldurationtype_as_ical_string(duration));

            break;
        }
        case ICAL_X_VALUE: {
            const char* x = icalvalue_get_x(val);
            rc = xmlTextWriterWriteFormatString(writer, "%s", x);
            break;
        }
        case ICAL_RECUR_VALUE: {
            struct icalrecurrencetype recur = icalvalue_get_recur(val);

            rv = icalrecurrencetype_to_xml(f, &recur, writer);
            if (rv != APR_SUCCESS) {
                return rv;
            }

            break;
        }
        case ICAL_TRIGGER_VALUE: {
            struct icaltriggertype trigger = icalvalue_get_trigger(val);

            if (!icaltime_is_null_time(trigger.time)) {
                icaltime_to_xml("time", trigger.time, writer);
            }
            else {
                icalduration_to_xml("duration", trigger.duration, writer);
            }

            break;
        }
        case ICAL_DATE_VALUE: {
            struct icaltimetype date = icalvalue_get_date(val);
            rc = xmlTextWriterWriteFormatString(writer, "%04d-%02d-%02d",
                    date.year, date.month, date.day);

            break;
        }
        case ICAL_DATETIME_VALUE: {
            struct icaltimetype datetime = icalvalue_get_datetime(val);
            rc = xmlTextWriterWriteFormatString(writer, "%04d-%02d-%02dT%02d:%02d:%02d",
                    datetime.year, datetime.month, datetime.day, datetime.hour,
                    datetime.minute, datetime.second);

            break;
        }
        default: {
            /* if we don't recognise it, write it as a string */
            char *str = icalvalue_as_ical_string_r(val);
            rc = xmlTextWriterWriteFormatString(writer, "%s", str);
            icalmemory_free_buffer(str);

            break;
        }
        }
        if (rc < 0) {
            return APR_EGENERAL;
        }

        /* close property element */
        rc = xmlTextWriterEndElement(writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

    }

    return rv;
}

static apr_status_t icalparameter_to_json(ap_filter_t *f, icalparameter *param,
        json_object *jobject)
{
    apr_status_t rv = APR_SUCCESS;

    if (param) {
        char *element;
        const char *str;
        icalparameter_kind kind = icalparameter_isa(param);

        /* work out the parameter name */
        if (kind == ICAL_X_PARAMETER) {
            element = apr_pstrdup(f->r->pool, icalparameter_get_xname(param));
        }
#ifdef ICAL_IANA_PARAMETER
        else if (kind == ICAL_IANA_PARAMETER) {
            element = apr_pstrdup(f->r->pool,
                    icalparameter_get_iana_name(param));
        }
#endif
        else {
            element = apr_pstrdup(f->r->pool,
                    icalparameter_kind_to_string(kind));
        }

        /* write parameter */
        str = icalparameter_get_xvalue(param);
        if (str) {
            json_object_object_add(jobject, strlwr(element),
                    json_object_new_string(str));
        }

    }

    return rv;
}

static apr_status_t icalparameter_to_xml(ap_filter_t *f, icalparameter *param,
        xmlTextWriterPtr writer)
{
    apr_status_t rv = APR_SUCCESS;
    int rc;

    if (param) {
        char *element;
        const char *str;
        icalparameter_kind kind = icalparameter_isa(param);

        /* work out the parameter name */
        if (kind == ICAL_X_PARAMETER) {
            element = apr_pstrdup(f->r->pool, icalparameter_get_xname(param));
        }
#ifdef ICAL_IANA_PARAMETER
        else if (kind == ICAL_IANA_PARAMETER) {
            element = apr_pstrdup(f->r->pool,
                    icalparameter_get_iana_name(param));
        }
#endif
        else {
            element = apr_pstrdup(f->r->pool,
                    icalparameter_kind_to_string(kind));
        }
        element = strlwr(element);

        /* write parameter */
        str = icalparameter_get_xvalue(param);
        if (str) {
            rc = xmlTextWriterWriteFormatElement(writer, BAD_CAST
                    element, "%s", str);
        }

    }

    return rv;
}

static apr_status_t icalproperty_to_json(ap_filter_t *f, icalproperty *prop,
        json_object *jarray)
{
    apr_status_t rv = APR_SUCCESS;

    if (prop) {
        char *element;
        const char *x_name;
        icalparameter *sparam;
        icalproperty_kind kind = icalproperty_isa(prop);
        json_object *jprop, *jparam;

        jprop = json_object_new_array();
        json_object_array_add(jarray, jprop);

        /* work out the parameter name */
        x_name = icalproperty_get_x_name(prop);
        if (kind == ICAL_X_PROPERTY && x_name != 0) {
            element = apr_pstrdup(f->r->pool, x_name);
        }
        else {
            element = apr_pstrdup(f->r->pool,
                    icalproperty_kind_to_string(kind));
        }

        /* open property element */
        json_object_array_add(jprop, json_object_new_string(strlwr(element)));

        /* handle parameters */
        jparam = json_object_new_object();
        json_object_array_add(jprop, jparam);
        sparam = icalproperty_get_first_parameter(prop, ICAL_ANY_PARAMETER);
        if (sparam) {

            while (sparam) {

                rv = icalparameter_to_json(f, sparam, jparam);
                if (rv != APR_SUCCESS) {
                    return rv;
                }

                sparam = icalproperty_get_next_parameter(prop, ICAL_ANY_PARAMETER);
            }

        }

        /* handle value */
        switch (kind) {
        case ICAL_CATEGORIES_PROPERTY:
        case ICAL_RESOURCES_PROPERTY:
        case ICAL_FREEBUSY_PROPERTY:
        case ICAL_EXDATE_PROPERTY:
        case ICAL_RDATE_PROPERTY: {
            rv = icalvalue_multi_to_json(f, icalproperty_get_value(prop), jprop);
            if (rv != APR_SUCCESS) {
                return rv;
            }
            break;
        }
        default: {
            rv = icalvalue_to_json(f, icalproperty_get_value(prop), jprop);
            if (rv != APR_SUCCESS) {
                return rv;
            }
            break;
        }
        }

    }

    return rv;
}

static apr_status_t icalproperty_to_xml(ap_filter_t *f, icalproperty *prop,
        xmlTextWriterPtr writer)
{
    apr_status_t rv = APR_SUCCESS;
    int rc;

    if (prop) {
        char *element;
        const char *x_name;
        icalparameter *sparam;
        icalproperty_kind kind = icalproperty_isa(prop);

        /* work out the parameter name */
        x_name = icalproperty_get_x_name(prop);
        if (kind == ICAL_X_PROPERTY && x_name != 0) {
            element = apr_pstrdup(f->r->pool, x_name);
        }
        else {
            element = apr_pstrdup(f->r->pool,
                    icalproperty_kind_to_string(kind));
        }

        /* open property element */
        element = strlwr(element);
        rc = xmlTextWriterStartElement(writer, BAD_CAST
                element);
        if (rc < 0) {
            return APR_EGENERAL;
        }

        /* handle parameters */
        sparam = icalproperty_get_first_parameter(prop, ICAL_ANY_PARAMETER);
        if (sparam) {

            rc = xmlTextWriterStartElement(writer, BAD_CAST "parameters");
            if (rc < 0) {
                return APR_EGENERAL;
            }

            while (sparam) {

                rv = icalparameter_to_xml(f, sparam, writer);
                if (rv != APR_SUCCESS) {
                    return rv;
                }

                sparam = icalproperty_get_next_parameter(prop, ICAL_ANY_PARAMETER);
            }

            rc = xmlTextWriterEndElement(writer);
            if (rc < 0) {
                return APR_EGENERAL;
            }

        }

        /* handle value */
        switch (kind) {
        case ICAL_CATEGORIES_PROPERTY:
        case ICAL_RESOURCES_PROPERTY:
        case ICAL_FREEBUSY_PROPERTY:
        case ICAL_EXDATE_PROPERTY:
        case ICAL_RDATE_PROPERTY: {
            rv = icalvalue_multi_to_xml(f,
                    icalproperty_get_value(prop), writer);
            if (rv != APR_SUCCESS) {
                return rv;
            }
            break;
        }
        default: {
            rv = icalvalue_to_xml(f, icalproperty_get_value(prop), writer);
            if (rv != APR_SUCCESS) {
                return rv;
            }
            break;
        }
        }

        /* close property element */
        rc = xmlTextWriterEndElement(writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

    }

    return rv;
}

static apr_status_t icalcomponent_to_json(ap_filter_t *f, icalcomponent *comp,
        json_object * array)
{
    apr_status_t rv = APR_SUCCESS;

    if (comp) {
        icalcomponent *scomp;
        icalproperty *sprop;
        char *element;
        json_object *jprop, *jcomp;

        /* open component element */
        element = apr_pstrdup(f->r->pool,
                icalcomponent_kind_to_string(icalcomponent_isa(comp)));
        json_object_array_add(array, json_object_new_string(strlwr(element)));

        /* handle properties */
        jprop = json_object_new_array();
        json_object_array_add(array, jprop);
        sprop = icalcomponent_get_first_property(comp, ICAL_ANY_PROPERTY);
        if (sprop) {

            while (sprop) {

                rv = icalproperty_to_json(f, sprop, jprop);
                if (rv != APR_SUCCESS) {
                    return rv;
                }

                sprop = icalcomponent_get_next_property(comp, ICAL_ANY_PROPERTY);
            }

        }

        /* handle components */
        jcomp = json_object_new_array();
        json_object_array_add(array, jcomp);
        scomp = icalcomponent_get_first_component (comp, ICAL_ANY_COMPONENT);
        if (scomp) {

            while (scomp) {

                rv = icalcomponent_to_json(f, scomp, jcomp);
                if (rv != APR_SUCCESS) {
                    return rv;
                }

                scomp = icalcomponent_get_next_component (comp, ICAL_ANY_COMPONENT);
            }

        }

    }

    return rv;
}

static apr_status_t icalcomponent_to_xml(ap_filter_t *f, icalcomponent *comp,
        xmlTextWriterPtr writer)
{
    apr_status_t rv = APR_SUCCESS;
    int rc;

    if (comp) {
        icalcomponent *scomp;
        icalproperty *sprop;
        char *element;

        /* open component element */
        element = apr_pstrdup(f->r->pool,
                icalcomponent_kind_to_string(icalcomponent_isa(comp)));
        rc = xmlTextWriterStartElement(writer, BAD_CAST
                strlwr(element));
        if (rc < 0) {
            return APR_EGENERAL;
        }

        /* handle properties */
        sprop = icalcomponent_get_first_property(comp, ICAL_ANY_PROPERTY);
        if (sprop) {

            rc = xmlTextWriterStartElement(writer, BAD_CAST "properties");
            if (rc < 0) {
                return APR_EGENERAL;
            }

            while (sprop) {

                rv = icalproperty_to_xml(f, sprop, writer);
                if (rv != APR_SUCCESS) {
                    return rv;
                }

                sprop = icalcomponent_get_next_property(comp, ICAL_ANY_PROPERTY);
            }

            rc = xmlTextWriterEndElement(writer);
            if (rc < 0) {
                return APR_EGENERAL;
            }

        }

        /* handle components */
        scomp = icalcomponent_get_first_component (comp, ICAL_ANY_COMPONENT);
        if (scomp) {

            rc = xmlTextWriterStartElement(writer, BAD_CAST "components");
            if (rc < 0) {
                return APR_EGENERAL;
            }

            while (scomp) {

                rv = icalcomponent_to_xml(f, scomp, writer);
                if (rv != APR_SUCCESS) {
                    return rv;
                }

                scomp = icalcomponent_get_next_component (comp, ICAL_ANY_COMPONENT);
            }

            rc = xmlTextWriterEndElement(writer);
            if (rc < 0) {
                return APR_EGENERAL;
            }

        }

        /* close component element */
        rc = xmlTextWriterEndElement(writer);
        if (rc < 0) {
            return APR_EGENERAL;
        }

    }

    return rv;
}

static apr_status_t ical_to_xcal(ap_filter_t *f, icalcomponent *comp)
{
    apr_status_t rv;
    int rc;
    ical_ctx *ctx = f->ctx;
    xmlBufferPtr buf;
    xmlTextWriterPtr writer;

    buf = xmlBufferCreate();
    if (buf == NULL) {
        return APR_ENOMEM;
    }
    apr_pool_cleanup_register(f->r->pool, buf, xmlbuffer_cleanup,
            apr_pool_cleanup_null);

    writer = xmlNewTextWriterMemory(buf, 0);
    if (writer == NULL) {
        return APR_ENOMEM;
    }
    apr_pool_cleanup_register(f->r->pool, writer, xmlwriter_cleanup,
            apr_pool_cleanup_null);

    if (ctx->format == AP_ICAL_FORMAT_PRETTY
            || ctx->format == AP_ICAL_FORMAT_SPACED) {
        xmlTextWriterSetIndent(writer, 1);
        xmlTextWriterSetIndentString    (writer, BAD_CAST "  ");
    }

    rc = xmlTextWriterStartDocument(writer, NULL, "UTF-8", NULL);
    if (rc < 0) {
        return APR_EGENERAL;
    }

    rc = xmlTextWriterStartElementNS(writer, NULL, BAD_CAST "icalendar", BAD_CAST "urn:ietf:params:xml:ns:icalendar-2.0");
    if (rc < 0) {
        return APR_EGENERAL;
    }

    rv = icalcomponent_to_xml(f, comp, writer);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    rc = xmlTextWriterEndElement(writer);
    if (rc < 0) {
        return APR_EGENERAL;
    }

    rc = xmlTextWriterEndDocument(writer);
    if (rc < 0) {
        return APR_EGENERAL;
    }

    apr_pool_cleanup_run(f->r->pool, writer, xmlwriter_cleanup);

    rv = apr_brigade_puts(ctx->bb, NULL, NULL, (const char *) buf->content);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    apr_pool_cleanup_run(f->r->pool, buf, xmlbuffer_cleanup);

    return APR_SUCCESS;
}

static apr_status_t ical_to_jcal(ap_filter_t *f, icalcomponent *comp)
{
    apr_status_t rv;
    ical_ctx *ctx = f->ctx;
    json_object * jarray;
    const char *str;

    jarray = json_object_new_array();
    if (jarray == NULL) {
        return APR_ENOMEM;
    }
    apr_pool_cleanup_register(f->r->pool, jarray, jsonbuffer_cleanup,
            apr_pool_cleanup_null);

    rv = icalcomponent_to_json(f, comp, jarray);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    str = json_object_to_json_string_ext(jarray,
            ctx->format == AP_ICAL_FORMAT_PRETTY ? JSON_C_TO_STRING_PRETTY :
            ctx->format == AP_ICAL_FORMAT_SPACED ? JSON_C_TO_STRING_SPACED :
                    JSON_C_TO_STRING_PLAIN);
    rv = apr_brigade_puts(ctx->bb, NULL, NULL, str);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    apr_pool_cleanup_run(f->r->pool, jarray, jsonbuffer_cleanup);

    return APR_SUCCESS;
}

static apr_status_t ical_to_ical(ap_filter_t *f, icalcomponent *comp)
{
    apr_status_t rv;
    char *temp;
    ical_ctx *ctx = f->ctx;

    temp = icalcomponent_as_ical_string_r(comp);
    rv = apr_brigade_write(ctx->bb, NULL, NULL, temp, strlen(temp));
    free(temp);

    return rv;
}

static ap_ical_filter_e parse_filter(const char *arg, apr_off_t len)
{
    if (!strncmp(arg, "none", len)) {
        return AP_ICAL_FILTER_NONE;
    }
    else if (!strncmp(arg, "next", len)) {
        return AP_ICAL_FILTER_NEXT;
    }
    else if (!strncmp(arg, "last", len)) {
        return AP_ICAL_FILTER_LAST;
    }
    else if (!strncmp(arg, "future", len)) {
        return AP_ICAL_FILTER_FUTURE;
    }
    else if (!strncmp(arg, "past", len)) {
        return AP_ICAL_FILTER_PAST;
    }
    else {
        return AP_ICAL_FILTER_UNKNOWN;
    }
}

static ap_ical_format_e parse_format(const char *arg, apr_off_t len)
{
    if (!strncmp(arg, "none", len)) {
        return AP_ICAL_FORMAT_NONE;
    }
    else if (!strncmp(arg, "pretty", len)) {
        return AP_ICAL_FORMAT_PRETTY;
    }
    else if (!strncmp(arg, "spaced", len)) {
        return AP_ICAL_FORMAT_SPACED;
    }
    else {
        return AP_ICAL_FORMAT_UNKNOWN;
    }
}

static icalcomponent *filter_component(ap_filter_t *f, icalcomponent *comp)
{
    ical_ctx *ctx = f->ctx;

    if (comp) {
        icalcomponent *scomp, *candidate = NULL;

        icalcompiter iter = icalcomponent_begin_component(comp,
                ICAL_ANY_COMPONENT);

        struct icaltimetype now = icaltime_current_time_with_zone(
                icaltimezone_get_utc_timezone());

        while ((scomp = icalcompiter_next(&iter))) {

            switch (ctx->filter) {
            case AP_ICAL_FILTER_NEXT: {
                struct icaltimetype end = icalcomponent_get_dtend(scomp);

                /* in the past? */
                if (icaltime_compare(now, end) > 0) {
                    icalcompiter_next(&iter);
                    icalcomponent_remove_component(comp, scomp);
                    icalcomponent_free(scomp);
                    break;
                }

                /* better than candidate? */
                if (candidate) {
                    if (icaltime_compare(end,
                            icalcomponent_get_dtend(candidate)) < 0) {
                        icalcompiter_next(&iter);
                        icalcomponent_remove_component(comp, candidate);
                        icalcomponent_free(candidate);
                    }
                }

                /* we are now the best candidate */
                candidate = scomp;
                break;
            }
            case AP_ICAL_FILTER_LAST: {
                struct icaltimetype end = icalcomponent_get_dtend(scomp);

                /* in the future? */
                if (icaltime_compare(now, end) < 0) {
                    icalcompiter_next(&iter);
                    icalcomponent_remove_component(comp, scomp);
                    icalcomponent_free(scomp);
                    break;
                }

                /* better than candidate? */
                if (candidate) {
                    if (icaltime_compare(end,
                            icalcomponent_get_dtend(candidate)) > 0) {
                        icalcompiter_next(&iter);
                        icalcomponent_remove_component(comp, candidate);
                        icalcomponent_free(candidate);
                    }
                }

                /* we are now the best candidate */
                candidate = scomp;
                break;
            }
            case AP_ICAL_FILTER_FUTURE: {
                struct icaltimetype end = icalcomponent_get_dtend(scomp);

                /* in the past? */
                if (icaltime_compare(now, end) > 0) {
                    icalcompiter_next(&iter);
                    icalcomponent_remove_component(comp, scomp);
                    icalcomponent_free(scomp);
                    break;
                }

                break;
            }
            case AP_ICAL_FILTER_PAST: {
                struct icaltimetype end = icalcomponent_get_dtend(scomp);

                /* in the future? */
                if (icaltime_compare(now, end) < 0) {
                    icalcompiter_next(&iter);
                    icalcomponent_remove_component(comp, scomp);
                    icalcomponent_free(scomp);
                    break;
                }

                break;
            }
            default: {
                /* none, passthrough */
                break;
            }
            }

        }

    }

    return comp;
}

static icalcomponent *add_line(ap_filter_t *f, ical_ctx *ctx)
{
    char *buffer;
    apr_off_t actual;
    apr_size_t total;
    icalcomponent *comp;

    /* flatten the brigade, terminate with NUL */
    apr_brigade_length(ctx->tmp, 1, &actual);

    total = (apr_size_t) actual;

    buffer = apr_palloc(f->r->pool, total + 1);
    buffer[total] = 0;
    apr_brigade_flatten(ctx->tmp, buffer, &total);
    apr_brigade_cleanup(ctx->tmp);

    /* handle line in ctx->tmp */
    comp = icalparser_add_line(ctx->parser, buffer);

    /* clean up the component */
    if (comp) {
        apr_pool_cleanup_register(f->r->pool, comp, icalcomponent_cleanup,
                apr_pool_cleanup_null);
    }

    return comp;
}

static apr_status_t ical_header(ap_filter_t *f)
{
    ical_ctx *ctx = f->ctx;

    switch (ctx->output) {
    case AP_ICAL_OUTPUT_ICAL: {
        break;
    }
    case AP_ICAL_OUTPUT_XCAL: {
        ap_set_content_type(f->r, "application/calendar+xml");
        break;
    }
    case AP_ICAL_OUTPUT_JCAL: {
        ap_set_content_type(f->r, "application/calendar+json");
        break;
    }
    default: {
        break;
    }
    }

    return APR_SUCCESS;
}

static apr_status_t ical_footer(ap_filter_t *f)
{
    return APR_SUCCESS;
}

static apr_status_t ical_query(ap_filter_t *f)
{
    ical_ctx *ctx = f->ctx;
    const char *slider = f->r->args;

    ical_conf *conf = ap_get_module_config(f->r->per_dir_config,
            &ical_module);

    ctx->filter = conf->filter;
    ctx->format = conf->format;

    while (slider && *slider) {
        const char *key = slider;
        const char *val = ap_strchr(slider, '=');
        apr_off_t klen, vlen;

        slider = ap_strchr(slider, '&');

        if (val) {

            /* isolate the key and value */
            klen = val - key;
            val++;
            if (slider) {
                vlen = slider - val;
                slider++;
            }
            else {
                vlen = strlen(val);
            }

            /* what have we found? */
            if (!strncmp(key, "filter", klen)) {

                ap_ical_filter_e filter = parse_filter(val, vlen);
                if (filter != AP_ICAL_FILTER_UNKNOWN) {
                    ctx->filter = filter;
                }

            }

            if (!strncmp(key, "format", klen)) {

                ap_ical_format_e format = parse_format(val, vlen);
                if (format != AP_ICAL_FORMAT_UNKNOWN) {
                    ctx->format = format;
                }

            }

        }

    };

    return APR_SUCCESS;
}

static apr_status_t ical_write(ap_filter_t *f, icalcomponent *comp)
{
    ical_ctx *ctx = f->ctx;
    apr_status_t rv;

    switch (ctx->output) {
    case AP_ICAL_OUTPUT_ICAL: {
        rv = ical_to_ical(f, comp);
        break;
    }
    case AP_ICAL_OUTPUT_XCAL: {
        rv = ical_to_xcal(f, comp);
        break;
    }
    case AP_ICAL_OUTPUT_JCAL: {
        rv = ical_to_jcal(f, comp);
        break;
    }
    default: {
        rv = APR_ENOTIMPL;
        break;
    }
    }

    return rv;
}

static int ical_out_setup(ap_filter_t *f)
{
    ical_ctx *ctx;

    ctx = f->ctx = apr_pcalloc(f->r->pool, sizeof(ical_ctx));
    ctx->output = AP_ICAL_OUTPUT_NEGOTIATED;

    return APR_SUCCESS;
}

static int ical_out_ical_setup(ap_filter_t *f)
{
    ical_ctx *ctx;

    ctx = f->ctx = apr_pcalloc(f->r->pool, sizeof(ical_ctx));
    ctx->output = AP_ICAL_OUTPUT_ICAL;

    return APR_SUCCESS;
}

static int ical_out_xcal_setup(ap_filter_t *f)
{
    ical_ctx *ctx;

    ctx = f->ctx = apr_pcalloc(f->r->pool, sizeof(ical_ctx));
    ctx->output = AP_ICAL_OUTPUT_XCAL;

    return APR_SUCCESS;
}

static int ical_out_jcal_setup(ap_filter_t *f)
{
    ical_ctx *ctx;

    ctx = f->ctx = apr_pcalloc(f->r->pool, sizeof(ical_ctx));
    ctx->output = AP_ICAL_OUTPUT_JCAL;

    return APR_SUCCESS;
}

static apr_status_t ical_out_filter(ap_filter_t *f, apr_bucket_brigade *bb)
{
    apr_status_t rv = APR_SUCCESS;
    apr_bucket *e;
    request_rec *r = f->r;
    ical_ctx *ctx = f->ctx;
    icalcomponent *comp;

    /* first time in? create a parser */
    if (!ctx->parser) {

        /* sanity check - input must be text/calendar or fail */
        if (ctx->output == AP_ICAL_OUTPUT_NEGOTIATED) {
            const char *ct;
            ct = ap_field_noparam(r->pool,
                    apr_table_get(r->headers_out, "Content-Type"));
            if (!ct || strcasecmp(ct, "text/calendar")) {
                ap_log_rerror(APLOG_MARK, APLOG_WARNING, APR_SUCCESS, r,
                        "unexpected content-type '%s', %s filter needs 'text/calendar', filter disabled",
                        ct, f->frec->name);
                ap_remove_output_filter(f);
                return ap_pass_brigade(f->next, bb);
            }
        }

        ctx->bb = apr_brigade_create(r->pool, f->c->bucket_alloc);
        ctx->tmp = apr_brigade_create(r->pool, f->c->bucket_alloc);

        ctx->parser = icalparser_new();
        apr_pool_cleanup_register(r->pool, ctx->parser, icalparser_cleanup,
                apr_pool_cleanup_null);

        /* must we negotiate the output format? */
        if (ctx->output == AP_ICAL_OUTPUT_NEGOTIATED) {
            const char *accept = apr_table_get(r->headers_in, "Accept");

            if (!accept) {
                /* fall back to text/calendar by default */
                ctx->output = AP_ICAL_OUTPUT_ICAL;
            }
            else if (!strcmp(accept, "text/calendar")) {
                ctx->output = AP_ICAL_OUTPUT_ICAL;
            }
            else if (!strcmp(accept, "application/calendar+xml")) {
                ctx->output = AP_ICAL_OUTPUT_XCAL;
            }
            else if (!strcmp(accept, "application/calendar+json")) {
                ctx->output = AP_ICAL_OUTPUT_JCAL;
            }
            else {
                /* fall back to text/calendar by default */
                ctx->output = AP_ICAL_OUTPUT_ICAL;
            }
            apr_table_merge(r->headers_out, "Vary", "Accept");
        }

        /* type of filtering/formatting to do */
        ical_query(f);

        rv = ical_header(f);
        if (rv != APR_SUCCESS) {
            return rv;
        }

    }

    while (APR_SUCCESS == rv && !APR_BRIGADE_EMPTY(bb)) {
        const char *data;
        apr_size_t size;

        e = APR_BRIGADE_FIRST(bb);

        /* EOS means we are done. */
        if (APR_BUCKET_IS_EOS(e)) {

            /* handle last line */
            comp = filter_component(f, add_line(f, ctx));
            if (comp) {

                rv = ical_write(f, comp);
                if (rv != APR_SUCCESS) {
                    return rv;
                }

                rv = ical_footer(f);
                if (rv != APR_SUCCESS) {
                    return rv;
                }

                apr_pool_cleanup_run(f->r->pool, comp, icalcomponent_cleanup);
            }

            /* pass the EOS across */
            APR_BRIGADE_CONCAT(ctx->bb, bb);

            /* pass what we have down the chain */
            ap_remove_output_filter(f);
            return ap_pass_brigade(f->next, ctx->bb);
        }

        /* metadata buckets are preserved as is */
        if (APR_BUCKET_IS_METADATA(e)) {
            /*
             * Remove meta data bucket from old brigade and insert into the
             * new.
             */
            APR_BUCKET_REMOVE(e);
            APR_BRIGADE_INSERT_TAIL(ctx->bb, e);
            continue;
        }

        /* at this point we are ready to buffer.
         * Buffering takes advantage of an optimisation in the handling of
         * bucket brigades. Heap buckets are always created at a fixed
         * size, regardless of the size of the data placed into them.
         * The apr_brigade_write() call will first try and pack the data
         * into any free space in the most recent heap bucket, before
         * allocating a new bucket if necessary.
         */
        if (APR_SUCCESS == (rv = apr_bucket_read(e, &data, &size,
                APR_BLOCK_READ))) {
            const char *poslf, *poscr;

            if (!size) {
                apr_bucket_delete(e);
                continue;
            }

            if (ctx->eat_crlf) {
                if (*data == APR_ASCII_CR || *data == APR_ASCII_LF) {
                    apr_bucket_split(e, 1);
                    apr_bucket_delete(e);
                    continue;
                }
                ctx->eat_crlf = 0;
            }

            if (ctx->seen_eol) {
                ctx->seen_eol = 0;

                /* continuation line? */
                if (size
                        && (*data == APR_ASCII_BLANK || *data == APR_ASCII_TAB)) {
                    apr_bucket_split(e, 1);
                    apr_bucket_delete(e);
                    continue;
                }

                /* process the line */
                else if (!APR_BRIGADE_EMPTY(ctx->tmp)) {
                    comp = filter_component(f, add_line(f, ctx));
                    if (comp) {

                        rv = ical_write(f, comp);
                        if (rv != APR_SUCCESS) {
                            return rv;
                        }

                        rv = ap_pass_brigade(f->next, ctx->bb);
                        apr_pool_cleanup_run(f->r->pool, comp,
                                icalcomponent_cleanup);
                    }
                    continue;
                }

            }

            /* end of line? */
            poscr = memchr(data, APR_ASCII_CR, size);
            poslf = memchr(data, APR_ASCII_LF, size);
            if (poslf || poscr) {
                const char *pos = (!poslf) ? poscr : (!poscr) ? poslf :
                                  (poslf < poscr) ? poslf : poscr;

                /* isolate the string */
                if (pos != data) {
                    apr_bucket_split(e, pos - data);
                    apr_bucket_setaside(e, f->r->pool);
                }
                APR_BUCKET_REMOVE(e);
                APR_BRIGADE_INSERT_TAIL(ctx->tmp, e);

                ctx->eat_crlf = 1;
                ctx->seen_eol = 1;
            }
            else {
                APR_BUCKET_REMOVE(e);
                APR_BRIGADE_INSERT_TAIL(ctx->tmp, e);
            }

        }

    }

    return rv;
}

static void *create_ical_config(apr_pool_t *p, char *dummy)
{
    ical_conf *new = (ical_conf *) apr_pcalloc(p, sizeof(ical_conf));

    new->filter = DEFAULT_ICAL_FILTER; /* default filter */
    new->format = DEFAULT_ICAL_FORMAT; /* default format */

    return (void *) new;
}

static void *merge_ical_config(apr_pool_t *p, void *basev, void *addv)
{
    ical_conf *new = (ical_conf *) apr_pcalloc(p, sizeof(ical_conf));
    ical_conf *add = (ical_conf *) addv;
    ical_conf *base = (ical_conf *) basev;

    new->filter = (add->filter_set == 0) ? base->filter : add->filter;
    new->filter_set = add->filter_set || base->filter_set;
    new->format = (add->format_set == 0) ? base->format : add->format;
    new->format_set = add->format_set || base->format_set;

    return new;
}

static const char *set_ical_filter(cmd_parms *cmd, void *dconf, const char *arg)
{
    ical_conf *conf = dconf;

    conf->filter = parse_filter(arg, strlen(arg));

    if (conf->filter == AP_ICAL_FILTER_UNKNOWN) {
        return "ICalFilter must be one of 'none', 'next', 'last', future' or 'past'";
    }

    conf->filter_set = 1;

    return NULL;
}

static const char *set_ical_format(cmd_parms *cmd, void *dconf, const char *arg)
{
    ical_conf *conf = dconf;

    conf->format = parse_format(arg, strlen(arg));

    if (conf->format == AP_ICAL_FORMAT_UNKNOWN) {
        return "ICalFormat must be one of 'none', 'spaced' or 'pretty'";
    }

    conf->format_set = 1;

    return NULL;
}

static const command_rec ical_cmds[] = {
    AP_INIT_TAKE1("ICalFilter", set_ical_filter, NULL, ACCESS_CONF,
        "Set the filtering to 'none', 'next', 'last', future' or 'past'. Defaults to 'past'"),
    AP_INIT_TAKE1("ICalFormat", set_ical_format, NULL, ACCESS_CONF,
        "Set the formatting to 'none', 'spaced' or 'pretty'. Defaults to 'none'"),
    { NULL }
};

static void ical_hooks(apr_pool_t* pool)
{
    ap_register_output_filter("ICAL", ical_out_filter, ical_out_setup,
            AP_FTYPE_RESOURCE);
    ap_register_output_filter("ICALICAL", ical_out_filter, ical_out_ical_setup,
            AP_FTYPE_RESOURCE);
    ap_register_output_filter("ICALXCAL", ical_out_filter, ical_out_xcal_setup,
            AP_FTYPE_RESOURCE);
    ap_register_output_filter("ICALJCAL", ical_out_filter, ical_out_jcal_setup,
            AP_FTYPE_RESOURCE);
}

module AP_MODULE_DECLARE_DATA ical_module = {
  STANDARD20_MODULE_STUFF,
  create_ical_config,
  merge_ical_config,
  NULL,
  NULL,
  ical_cmds,
  ical_hooks
};

