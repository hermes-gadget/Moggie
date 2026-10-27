#define _POSIX_C_SOURCE 200809L

#include "soap.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define SOAP_ENVELOPE_OPEN \
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" \
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" " \
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">" \
    "<s:Body>"
#define SOAP_ENVELOPE_CLOSE "</s:Body></s:Envelope>\n"

static int open_ok(struct sbuf *b, int service, const char *action)
{
    sb_puts(b, SOAP_ENVELOPE_OPEN);
    sb_printf(b, "<u:%sResponse xmlns:u=\"%s\">",
              action, dlna_svc_type[service]);
    return 0;
}

static void close_ok(struct sbuf *b, const char *action)
{
    sb_printf(b, "</u:%sResponse>", action);
    sb_puts(b, SOAP_ENVELOPE_CLOSE);
}

static int fault(char *out, size_t outsz, int code, const char *desc)
{
    struct sbuf b;

    sb_init(&b, out, outsz);
    sb_puts(&b, SOAP_ENVELOPE_OPEN);
    sb_puts(&b, "<s:Fault><faultcode>s:Client</faultcode>"
               "<faultstring>UPnPError</faultstring><detail>"
               "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">");
    sb_printf(&b, "<errorCode>%d</errorCode>", code);
    sb_puts(&b, "<errorDescription>");
    sb_escape(&b, desc);
    sb_puts(&b, "</errorDescription></UPnPError></detail></s:Fault>");
    sb_puts(&b, SOAP_ENVELOPE_CLOSE);
    if (!sb_ok(&b)) {
        log_error("soap: fault body overflow");
        return -1;
    }
    log_info("soap: UPnPError %d (%s)", code, desc);
    return (int)b.len;
}

static int err_401(char *out, size_t outsz, int *status)
{
    *status = 500;
    return fault(out, outsz, 401, "Invalid Action");
}

static int err_402(char *out, size_t outsz, int *status)
{
    *status = 500;
    return fault(out, outsz, 402, "Invalid Args");
}

static int err_701(char *out, size_t outsz, int *status)
{
    *status = 500;
    return fault(out, outsz, 701, "Transition not available");
}

static int instance_ok(const char *body)
{
    long id;

    if (xml_get_int(body, "InstanceID", &id) != 0)
        return 0;
    return id == 0;
}

static int channel_ok(const char *body)
{
    char ch[32];

    if (xml_get(body, "Channel", ch, sizeof(ch)) != 0)
        return 0;
    return str_ieq(ch, "Master");
}

static int get_volume_arg(const char *body, int *volume)
{
    long v;

    if (xml_get_int(body, "DesiredVolume", &v) != 0)
        return -1;
    if (v < 0 || v > 100)
        return -1;
    *volume = (int)v;
    return 0;
}

static int get_mute_arg(const char *body, int *mute)
{
    return xml_get_bool(body, "DesiredMute", mute);
}

static const char *actions_for(const struct dlna_playback *pb, int has_uri)
{
    switch (pb->state) {
    case DLNA_PLAYING: return "Stop,Pause,Seek";
    case DLNA_PAUSED:  return "Play,Stop,Seek";
    case DLNA_STOPPED: return has_uri ? "Play,Seek" : "Play";
    default:           return "";
    }
}

/* Map renderer failures onto the three faults this renderer uses. */
static int map_renderer_error(int rc, char *out, size_t outsz, int *status)
{
    if (rc == RENDERER_ERR_ARG)
        return err_402(out, outsz, status);
    return err_701(out, outsz, status);
}

/* -------- AVTransport --------------------------------------------------- */

static int avt_action(struct dlna_renderer *r, const char *action,
                      const char *body, char *out, size_t outsz,
                      int *status)
{
    struct sbuf b;
    struct dlna_playback pb;
    char hms[24];
    char uri[DLNA_MAX_URI];
    char meta[DLNA_MAX_METADATA];
    int rc;

    *status = 200;

    if (strcmp(action, "SetAVTransportURI") == 0) {
        if (!instance_ok(body))
            return err_402(out, outsz, status);
        if (xml_get(body, "CurrentURI", uri, sizeof(uri)) != 0 || !uri[0])
            return err_402(out, outsz, status);
        if (xml_get(body, "CurrentURIMetaData", meta, sizeof(meta)) != 0) {
            if (strstr(body, "CurrentURIMetaData"))
                return err_402(out, outsz, status);
            meta[0] = '\0';
        }
        rc = renderer_set_uri(r, uri, meta);
        if (rc != RENDERER_OK)
            return map_renderer_error(rc, out, outsz, status);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        close_ok(&b, action);
    } else if (strcmp(action, "Play") == 0) {
        char speed[16];

        if (!instance_ok(body))
            return err_402(out, outsz, status);
        if (xml_get(body, "Speed", speed, sizeof(speed)) == 0) {
            trim_ws(speed);
            if (speed[0] && strcmp(speed, "1") != 0)
                return err_701(out, outsz, status);
        }
        rc = renderer_play(r);
        if (rc != RENDERER_OK)
            return map_renderer_error(rc, out, outsz, status);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        close_ok(&b, action);
    } else if (strcmp(action, "Pause") == 0) {
        if (!instance_ok(body))
            return err_402(out, outsz, status);
        rc = renderer_pause(r);
        if (rc != RENDERER_OK)
            return map_renderer_error(rc, out, outsz, status);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        close_ok(&b, action);
    } else if (strcmp(action, "Stop") == 0) {
        if (!instance_ok(body))
            return err_402(out, outsz, status);
        rc = renderer_stop(r);
        if (rc != RENDERER_OK)
            return map_renderer_error(rc, out, outsz, status);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        close_ok(&b, action);
    } else if (strcmp(action, "Seek") == 0) {
        char unit[32];
        char target[32];
        unsigned pos;

        if (!instance_ok(body))
            return err_402(out, outsz, status);
        if (xml_get(body, "Unit", unit, sizeof(unit)) != 0 ||
            xml_get(body, "Target", target, sizeof(target)) != 0)
            return err_402(out, outsz, status);
        trim_ws(unit);
        if (strcmp(unit, "REL_TIME") != 0)
            return err_701(out, outsz, status);
        if (parse_hms(target, &pos) != 0)
            return err_402(out, outsz, status);
        rc = renderer_seek(r, pos);
        if (rc != RENDERER_OK)
            return map_renderer_error(rc, out, outsz, status);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        close_ok(&b, action);
    } else if (strcmp(action, "GetTransportInfo") == 0) {
        if (!instance_ok(body))
            return err_402(out, outsz, status);
        renderer_snapshot(r, &pb);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        sb_printf(&b, "<CurrentTransportState>%s</CurrentTransportState>",
                  dlna_transport_name(pb.state));
        sb_puts(&b, "<CurrentTransportStatus>OK</CurrentTransportStatus>");
        sb_puts(&b, "<CurrentSpeed>1</CurrentSpeed>");
        close_ok(&b, action);
    } else if (strcmp(action, "GetPositionInfo") == 0) {
        int track;

        if (!instance_ok(body))
            return err_402(out, outsz, status);
        renderer_snapshot(r, &pb);
        track = renderer_has_uri(r) ? 1 : 0;
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        sb_printf(&b, "<Track>%d</Track>", track);
        format_hms(hms, sizeof(hms), pb.duration_s);
        sb_printf(&b, "<TrackDuration>%s</TrackDuration>", hms);
        sb_puts(&b, "<TrackMetaData>");
        sb_escape(&b, renderer_metadata(r));
        sb_puts(&b, "</TrackMetaData>");
        sb_puts(&b, "<TrackURI>");
        sb_escape(&b, renderer_uri(r));
        sb_puts(&b, "</TrackURI>");
        format_hms(hms, sizeof(hms), pb.position_s);
        sb_printf(&b, "<RelTime>%s</RelTime>", hms);
        sb_printf(&b, "<AbsTime>%s</AbsTime>", hms);
        sb_puts(&b, "<RelCount>2147483647</RelCount>");
        sb_puts(&b, "<AbsCount>2147483647</AbsCount>");
        close_ok(&b, action);
    } else if (strcmp(action, "GetMediaInfo") == 0) {
        int tracks;

        if (!instance_ok(body))
            return err_402(out, outsz, status);
        renderer_snapshot(r, &pb);
        tracks = renderer_has_uri(r) ? 1 : 0;
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        sb_printf(&b, "<NrTracks>%d</NrTracks>", tracks);
        format_hms(hms, sizeof(hms), pb.duration_s);
        sb_printf(&b, "<MediaDuration>%s</MediaDuration>", hms);
        sb_puts(&b, "<CurrentURI>");
        sb_escape(&b, renderer_uri(r));
        sb_puts(&b, "</CurrentURI>");
        sb_puts(&b, "<CurrentURIMetaData>");
        sb_escape(&b, renderer_metadata(r));
        sb_puts(&b, "</CurrentURIMetaData>");
        sb_puts(&b, "<NextURI></NextURI><NextURIMetaData></NextURIMetaData>");
        sb_puts(&b, "<PlayMedium>NETWORK</PlayMedium>");
        sb_puts(&b, "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>");
        sb_puts(&b, "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>");
        close_ok(&b, action);
    } else if (strcmp(action, "GetTransportSettings") == 0) {
        if (!instance_ok(body))
            return err_402(out, outsz, status);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        sb_puts(&b, "<PlayMode>NORMAL</PlayMode>");
        sb_puts(&b, "<RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>");
        close_ok(&b, action);
    } else if (strcmp(action, "GetCurrentTransportActions") == 0) {
        if (!instance_ok(body))
            return err_402(out, outsz, status);
        renderer_snapshot(r, &pb);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        sb_printf(&b, "<Actions>%s</Actions>",
                  actions_for(&pb, renderer_has_uri(r)));
        close_ok(&b, action);
    } else if (strcmp(action, "GetDeviceCapabilities") == 0) {
        if (!instance_ok(body))
            return err_402(out, outsz, status);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_AVT, action);
        sb_puts(&b, "<PlayMedia>NETWORK</PlayMedia>");
        sb_puts(&b, "<RecMedia>NOT_IMPLEMENTED</RecMedia>");
        sb_puts(&b, "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
        close_ok(&b, action);
    } else {
        return err_401(out, outsz, status);
    }

    if (!sb_ok(&b)) {
        log_error("soap: response overflow for %s", action);
        return -1;
    }
    return (int)b.len;
}

/* -------- RenderingControl ---------------------------------------------- */

static int rcs_action(struct dlna_renderer *r, const char *action,
                      const char *body, char *out, size_t outsz,
                      int *status)
{
    struct sbuf b;
    struct dlna_playback pb;
    int rc;
    int volume;
    int mute;

    *status = 200;

    if (strcmp(action, "GetVolume") == 0) {
        if (!instance_ok(body) || !channel_ok(body))
            return err_402(out, outsz, status);
        renderer_snapshot(r, &pb);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_RCS, action);
        sb_printf(&b, "<CurrentVolume>%d</CurrentVolume>", pb.volume);
        close_ok(&b, action);
    } else if (strcmp(action, "SetVolume") == 0) {
        if (!instance_ok(body) || !channel_ok(body))
            return err_402(out, outsz, status);
        if (get_volume_arg(body, &volume) != 0)
            return err_402(out, outsz, status);
        rc = renderer_set_volume(r, volume);
        if (rc != RENDERER_OK)
            return map_renderer_error(rc, out, outsz, status);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_RCS, action);
        close_ok(&b, action);
    } else if (strcmp(action, "GetMute") == 0) {
        if (!instance_ok(body) || !channel_ok(body))
            return err_402(out, outsz, status);
        renderer_snapshot(r, &pb);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_RCS, action);
        sb_printf(&b, "<CurrentMute>%d</CurrentMute>", pb.mute ? 1 : 0);
        close_ok(&b, action);
    } else if (strcmp(action, "SetMute") == 0) {
        if (!instance_ok(body) || !channel_ok(body))
            return err_402(out, outsz, status);
        if (get_mute_arg(body, &mute) != 0)
            return err_402(out, outsz, status);
        rc = renderer_set_mute(r, mute);
        if (rc != RENDERER_OK)
            return map_renderer_error(rc, out, outsz, status);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_RCS, action);
        close_ok(&b, action);
    } else if (strcmp(action, "GetVolumeDB") == 0) {
        int db;

        if (!instance_ok(body) || !channel_ok(body))
            return err_402(out, outsz, status);
        renderer_snapshot(r, &pb);
        db = (pb.volume - 100) * 40 / 100;
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_RCS, action);
        sb_printf(&b, "<CurrentVolume>%d</CurrentVolume>", db * 256);
        close_ok(&b, action);
    } else {
        return err_401(out, outsz, status);
    }

    if (!sb_ok(&b)) {
        log_error("soap: response overflow for %s", action);
        return -1;
    }
    return (int)b.len;
}

/* -------- ConnectionManager --------------------------------------------- */

static int cm_action(struct dlna_renderer *r, const char *action,
                     const char *body, char *out, size_t outsz,
                     int *status)
{
    struct sbuf b;
    long conn_id;

    (void)r;
    *status = 200;

    if (strcmp(action, "GetProtocolInfo") == 0) {
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_CM, action);
        sb_puts(&b, "<Source></Source><Sink>");
        sb_escape(&b, DLNA_SINK_PROTOCOL_INFO);
        sb_puts(&b, "</Sink>");
        close_ok(&b, action);
    } else if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_CM, action);
        sb_puts(&b, "<ConnectionIDs>0</ConnectionIDs>");
        close_ok(&b, action);
    } else if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
        if (xml_get_int(body, "ConnectionID", &conn_id) != 0 || conn_id != 0)
            return err_402(out, outsz, status);
        sb_init(&b, out, outsz);
        open_ok(&b, DLNA_SVC_CM, action);
        sb_puts(&b, "<RcsID>0</RcsID><AVTransportID>0</AVTransportID>");
        sb_puts(&b, "<ProtocolInfo></ProtocolInfo>");
        sb_puts(&b, "<PeerConnectionManager></PeerConnectionManager>");
        sb_puts(&b, "<PeerConnectionID>-1</PeerConnectionID>");
        sb_puts(&b, "<Direction>Input</Direction><Status>OK</Status>");
        close_ok(&b, action);
    } else {
        return err_401(out, outsz, status);
    }

    if (!sb_ok(&b)) {
        log_error("soap: response overflow for %s", action);
        return -1;
    }
    return (int)b.len;
}

int soap_handle(struct dlna_renderer *r, struct gena *g, int service,
                const char *service_type, const char *action,
                const char *body, char *out, size_t outsz, int *status)
{
    (void)g;

    *status = 200;
    if (!service_type || strcmp(service_type, dlna_svc_type[service]) != 0) {
        log_info("soap: service mismatch (header '%s')",
                 service_type ? service_type : "");
        return err_401(out, outsz, status);
    }
    if (!action || !*action)
        return err_401(out, outsz, status);
    if (!body)
        return err_402(out, outsz, status);

    switch (service) {
    case DLNA_SVC_AVT:
        return avt_action(r, action, body, out, outsz, status);
    case DLNA_SVC_RCS:
        return rcs_action(r, action, body, out, outsz, status);
    case DLNA_SVC_CM:
        return cm_action(r, action, body, out, outsz, status);
    default:
        return err_401(out, outsz, status);
    }
}
