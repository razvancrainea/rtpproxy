/* Auto-generated by genfincode_stat.sh - DO NOT EDIT! */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "rtpp_types.h"
#include "rtpp_debug.h"
#include "rtpp_refcnt.h"
static void refcnt_attach_fin(void *pub) {
    fprintf(stderr, "Method refcnt_attach is called after destruction\x0a");
    abort();
}
static void refcnt_decref_fin(void *pub) {
    fprintf(stderr, "Method refcnt_decref is called after destruction\x0a");
    abort();
}
static void refcnt_getdata_fin(void *pub) {
    fprintf(stderr, "Method refcnt_getdata is called after destruction\x0a");
    abort();
}
static void refcnt_incref_fin(void *pub) {
    fprintf(stderr, "Method refcnt_incref is called after destruction\x0a");
    abort();
}
static void refcnt_reg_pd_fin(void *pub) {
    fprintf(stderr, "Method refcnt_reg_pd is called after destruction\x0a");
    abort();
}
static void refcnt_traceen_fin(void *pub) {
    fprintf(stderr, "Method refcnt_traceen is called after destruction\x0a");
    abort();
}
static const struct rtpp_refcnt_smethods rtpp_refcnt_smethods_fin = {
    . incref = (refcnt_incref_t)&refcnt_incref_fin,
    . decref = (refcnt_decref_t)&refcnt_decref_fin,
    . getdata = (refcnt_getdata_t)&refcnt_getdata_fin,
    . reg_pd = (refcnt_reg_pd_t)&refcnt_reg_pd_fin,
    . attach = (refcnt_attach_t)&refcnt_attach_fin,
    . traceen = (refcnt_traceen_t)&refcnt_traceen_fin,
};
void rtpp_refcnt_fin(struct rtpp_refcnt *pub) {
    RTPP_DBG_ASSERT(pub->smethods != &rtpp_refcnt_smethods_fin &&
      pub->smethods != NULL);
    pub->smethods = &rtpp_refcnt_smethods_fin;
}