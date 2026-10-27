#define _POSIX_C_SOURCE 200809L

#include "description.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

int description_build(const struct dlna_config *cfg, char *buf, size_t cap)
{
    struct sbuf b;
    int i;

    sb_init(&b, buf, cap);
    sb_puts(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    sb_puts(&b, "<root xmlns=\"urn:schemas-upnp-org:device-1-0\"");
    sb_printf(&b, " configId=\"%d\">\n", cfg->boot_id & 0xffff);
    sb_puts(&b, "<specVersion><major>1</major><minor>0</minor></specVersion>\n");
    sb_puts(&b, "<device>\n");
    sb_puts(&b, "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\n");
    sb_puts(&b, "<friendlyName>");
    sb_escape(&b, cfg->friendly_name);
    sb_puts(&b, "</friendlyName>\n");
    sb_puts(&b, "<manufacturer>Moggie</manufacturer>\n");
    sb_puts(&b, "<manufacturerURL>https://github.com/hermes-gadget/Moggie</manufacturerURL>\n");
    sb_puts(&b, "<modelDescription>Open firmware UPnP AV MediaRenderer</modelDescription>\n");
    sb_puts(&b, "<modelName>Moggie DLNA Renderer</modelName>\n");
    sb_printf(&b, "<modelNumber>%s</modelNumber>\n", DLNA_VERSION);
    sb_puts(&b, "<modelURL>https://github.com/hermes-gadget/Moggie</modelURL>\n");
    sb_puts(&b, "<serialNumber>MOGGIE-DLNA-1</serialNumber>\n");
    sb_printf(&b, "<UDN>uuid:%s</UDN>\n", cfg->uuid);
    sb_puts(&b, "<dlna:X_DLNADOC xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">DMR-1.50</dlna:X_DLNADOC>\n");
    sb_puts(&b, "<serviceList>\n");

    for (i = 0; i < DLNA_SVC_COUNT; i++) {
        sb_puts(&b, "<service>\n");
        sb_printf(&b, "<serviceType>%s</serviceType>\n", dlna_svc_type[i]);
        sb_printf(&b, "<serviceId>%s</serviceId>\n", dlna_svc_id[i]);
        sb_printf(&b, "<SCPDURL>/%s/scpd.xml</SCPDURL>\n", dlna_svc_dir[i]);
        sb_printf(&b, "<controlURL>/%s/control</controlURL>\n", dlna_svc_dir[i]);
        sb_printf(&b, "<eventSubURL>/%s/event</eventSubURL>\n", dlna_svc_dir[i]);
        sb_puts(&b, "</service>\n");
    }

    sb_puts(&b, "</serviceList>\n");
    sb_puts(&b, "</device>\n");
    sb_puts(&b, "</root>\n");

    if (!sb_ok(&b))
        return -1;
    return (int)b.len;
}
