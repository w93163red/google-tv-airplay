/* dnssd private backend: builds the same TXT records as UxPlay's lib/mdnsd backend
 * and hands registration to Kotlin's NsdManager through jni_bridge. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dnssd.h"
#include "dnssdint.h"
#include "utils.h"

#include "airscreen.h"

#define TXT_MAX 512

typedef struct {
    unsigned char bytes[TXT_MAX];
    int length;
} txt_t;

typedef struct {
    txt_t raop_txt;
    txt_t airplay_txt;
    char raop_name[256];
    bool raop_registered;
    bool airplay_registered;
} dnssd_private_t;

static int txt_add(txt_t *txt, const char *key, const char *value) {
    char item[256];
    int length = snprintf(item, sizeof(item), "%s=%s", key, value ? value : "");
    if (length < 0 || length > 255 || txt->length + length + 1 > TXT_MAX) {
        return -1;
    }
    txt->bytes[txt->length++] = (unsigned char) length;
    memcpy(txt->bytes + txt->length, item, (size_t) length);
    txt->length += length;
    return 0;
}

static int build_raop_txt(dnssd_t *d, txt_t *txt) {
    char features[22];
    snprintf(features, sizeof(features), "0x%X,0x%X", d->features1, d->features2);
    txt->length = 0;
    return txt_add(txt, "ch", RAOP_CH) ||
           txt_add(txt, "cn", RAOP_CN) ||
           txt_add(txt, "da", RAOP_DA) ||
           txt_add(txt, "et", RAOP_ET) ||
           txt_add(txt, "vv", RAOP_VV) ||
           txt_add(txt, "ft", features) ||
           txt_add(txt, "am", GLOBAL_MODEL) ||
           txt_add(txt, "md", RAOP_MD) ||
           txt_add(txt, "rhd", RAOP_RHD) ||
           txt_add(txt, "pw", "false") ||
           txt_add(txt, "sr", RAOP_SR) ||
           txt_add(txt, "ss", RAOP_SS) ||
           txt_add(txt, "sv", RAOP_SV) ||
           txt_add(txt, "tp", RAOP_TP) ||
           txt_add(txt, "txtvers", RAOP_TXTVERS) ||
           txt_add(txt, "sf", RAOP_SF) ||
           txt_add(txt, "vs", RAOP_VS) ||
           txt_add(txt, "vn", RAOP_VN) ||
           txt_add(txt, "pk", d->pk);
}

static int build_airplay_txt(dnssd_t *d, txt_t *txt) {
    char device_id[3 * MAX_HWADDR_LEN];
    char features[22];
    if (utils_hwaddr_airplay(device_id, sizeof(device_id), d->hw_addr, d->hw_addr_len) < 0) {
        return -1;
    }
    snprintf(features, sizeof(features), "0x%X,0x%X", d->features1, d->features2);
    txt->length = 0;
    return txt_add(txt, "deviceid", device_id) ||
           txt_add(txt, "features", features) ||
           txt_add(txt, "pw", "false") ||
           txt_add(txt, "flags", "0x4") ||
           txt_add(txt, "model", GLOBAL_MODEL) ||
           txt_add(txt, "pk", d->pk) ||
           txt_add(txt, "pi", AIRPLAY_PI) ||
           txt_add(txt, "srcvers", AIRPLAY_SRCVERS) ||
           txt_add(txt, "vv", AIRPLAY_VV);
}

/* AirScreen has no access-control UI, so pin_pw is always 0 and "pw" is always false. */
void *dnssd_private_init(dnssd_t *d, int *error) {
    if (error) *error = DNSSD_ERROR_NOERROR;
    dnssd_private_t *p = calloc(1, sizeof(*p));
    if (!p) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    char hwaddr[2 * MAX_HWADDR_LEN + 1];
    if (utils_hwaddr_raop(hwaddr, sizeof(hwaddr), d->hw_addr, d->hw_addr_len) < 0 ||
        snprintf(p->raop_name, sizeof(p->raop_name), "%s@%s", hwaddr, d->name) >= (int) sizeof(p->raop_name)) {
        free(p);
        if (error) *error = DNSSD_ERROR_HWADDRLEN;
        return NULL;
    }
    return p;
}

void dnssd_private_destroy(void *private) {
    free(private);
}

int dnssd_register_raop(dnssd_t *d, unsigned short port) {
    dnssd_private_t *p = d->dnssd_private;
    if (build_raop_txt(d, &p->raop_txt)) {
        return -1;
    }
    bridge_register_service(DNSSD_SERVICE_RAOP, p->raop_name, port, p->raop_txt.bytes, p->raop_txt.length);
    p->raop_registered = true;
    return 0;
}

int dnssd_register_airplay(dnssd_t *d, unsigned short port) {
    dnssd_private_t *p = d->dnssd_private;
    if (build_airplay_txt(d, &p->airplay_txt)) {
        return -1;
    }
    bridge_register_service(DNSSD_SERVICE_AIRPLAY, d->name, port, p->airplay_txt.bytes, p->airplay_txt.length);
    p->airplay_registered = true;
    return 0;
}

void dnssd_unregister_raop(dnssd_t *d) {
    dnssd_private_t *p = d->dnssd_private;
    if (p->raop_registered) {
        bridge_unregister_service(DNSSD_SERVICE_RAOP);
        p->raop_registered = false;
    }
}

void dnssd_unregister_airplay(dnssd_t *d) {
    dnssd_private_t *p = d->dnssd_private;
    if (p->airplay_registered) {
        bridge_unregister_service(DNSSD_SERVICE_AIRPLAY);
        p->airplay_registered = false;
    }
}

const char *dnssd_get_raop_txt(dnssd_t *d, int *length) {
    dnssd_private_t *p = d->dnssd_private;
    *length = p->raop_txt.length;
    return (const char *) p->raop_txt.bytes;
}

const char *dnssd_get_airplay_txt(dnssd_t *d, int *length) {
    dnssd_private_t *p = d->dnssd_private;
    *length = p->airplay_txt.length;
    return (const char *) p->airplay_txt.bytes;
}
