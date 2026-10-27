#ifndef MOGGIE_DLNA_SOAP_H
#define MOGGIE_DLNA_SOAP_H

#include "event.h"
#include "renderer.h"

/* Dispatch one SOAP control request. service_type and action come from the
 * SOAPACTION header. Fills out with the response body and sets *status to 200
 * or 500. Returns the body length, or -1 on response overflow. */
int soap_handle(struct dlna_renderer *r, struct gena *g, int service,
                const char *service_type, const char *action,
                const char *body, char *out, size_t outsz, int *status);

#endif /* MOGGIE_DLNA_SOAP_H */
