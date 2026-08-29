#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { DOC_PENDING, DOC_RUNNING, DOC_PASS, DOC_WARN, DOC_FAIL, DOC_SKIP } doc_state_t;

typedef struct {
    char        name[7];
    doc_state_t state;
    char        summary[16];        // fits after "[ok] NAME  " on a 26-col row
    char        detail[3][27];      // full-width lines for the detail page
} doc_check_t;

enum { DOC_LINK, DOC_DHCP, DOC_GW, DOC_DNS, DOC_WAN, DOC_PORTAL, DOC_TLS,
       DOC_IPV6, DOC_NAT, DOC_TIME, DOC_SPEED, DOC_N };

typedef struct {
    doc_check_t checks[DOC_N];
    int         current;            // index being run, -1 when idle
    bool        running, done;
    uint32_t    elapsed_ms;
    int         n_pass, n_warn, n_fail;
    char        activity[27];       // "resolving google.com"
    char        report_path[48];    // where the text report went, "" if no SD
} doctor_report_t;

void doctor_start(void);            // no-op while a run is in progress
void doctor_get(doctor_report_t *out);
bool doctor_busy(void);
