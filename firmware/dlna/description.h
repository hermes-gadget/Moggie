#ifndef MOGGIE_DLNA_DESCRIPTION_H
#define MOGGIE_DLNA_DESCRIPTION_H

#include "dlna.h"

/* Build /description.xml. Returns the body length, or -1 if it does not fit. */
int description_build(const struct dlna_config *cfg, char *buf, size_t cap);

#endif /* MOGGIE_DLNA_DESCRIPTION_H */
