#include "github.h"
#include "secrets.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "github";

#define RESP_MAX (64 * 1024)

static gh_stats_t        s_stats;
static SemaphoreHandle_t s_lock;

typedef struct { char *buf; int len; int cap; } resp_t;

static void lock_init(void) { if (!s_lock) s_lock = xSemaphoreCreateMutex(); }

static esp_err_t http_ev(esp_http_client_event_t *ev)
{
    if (ev->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    resp_t *r = (resp_t *)ev->user_data;
    if (!r) return ESP_OK;
    int n = ev->data_len;
    if (r->len + n >= r->cap) n = r->cap - r->len - 1;
    if (n > 0) {
        memcpy(r->buf + r->len, ev->data, n);
        r->len += n;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

// Performs one request and leaves the body in r->buf. Returns the HTTP status,
// or a negative value on transport failure.
static int gh_request(const char *url, const char *post_body, resp_t *r)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = http_ev,
        .user_data         = r,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,
        .method            = post_body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    esp_http_client_set_header(c, "User-Agent", "t-dongle-c5-dash");
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

    if (sizeof(GITHUB_TOKEN) > 1) {
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", GITHUB_TOKEN);
        esp_http_client_set_header(c, "Authorization", auth);
    }
    if (post_body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, post_body, strlen(post_body));
    }

    int status = -1;
    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) status = esp_http_client_get_status_code(c);
    else ESP_LOGE(TAG, "%s: %s", url, esp_err_to_name(err));

    esp_http_client_cleanup(c);
    return status;
}

// ------------------------------------------------------- GraphQL (token) ---
// Deliberately contains no double quotes, so it drops into a JSON string as-is.
static const char *GQL =
    "query($l:String!){user(login:$l){"
    "followers{totalCount} following{totalCount} "
    "repositories(first:100,ownerAffiliations:OWNER,isFork:false,"
    "orderBy:{field:STARGAZERS,direction:DESC}){totalCount nodes{stargazerCount}} "
    "pullRequests(states:OPEN){totalCount} issues(states:OPEN){totalCount} "
    "contributionsCollection{contributionCalendar{totalContributions "
    "weeks{contributionDays{weekday contributionCount}}}}}}";

static int level_for(int count, int max)
{
    if (count <= 0) return 0;
    if (max <= 0) return 1;
    if (count >= (max * 3) / 4) return 4;
    if (count >= max / 2)       return 3;
    if (count >= max / 4)       return 2;
    return 1;
}

static int total_of(cJSON *parent, const char *key)
{
    cJSON *obj = cJSON_GetObjectItem(parent, key);
    cJSON *tot = obj ? cJSON_GetObjectItem(obj, "totalCount") : NULL;
    return cJSON_IsNumber(tot) ? tot->valueint : 0;
}

static esp_err_t parse_graphql(const char *body, gh_stats_t *st)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) { snprintf(st->err, sizeof(st->err), "bad JSON"); return ESP_FAIL; }

    esp_err_t ret = ESP_FAIL;
    cJSON *errors = cJSON_GetObjectItem(root, "errors");
    if (errors && cJSON_GetArraySize(errors) > 0) {
        cJSON *msg = cJSON_GetObjectItem(cJSON_GetArrayItem(errors, 0), "message");
        snprintf(st->err, sizeof(st->err), "%s",
                 cJSON_IsString(msg) ? msg->valuestring : "graphql error");
        goto done;
    }

    cJSON *u = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "user");
    if (!cJSON_IsObject(u)) { snprintf(st->err, sizeof(st->err), "no such user"); goto done; }

    st->followers   = total_of(u, "followers");
    st->following   = total_of(u, "following");
    st->repos       = total_of(u, "repositories");
    st->open_prs    = total_of(u, "pullRequests");
    st->open_issues = total_of(u, "issues");

    cJSON *nodes = cJSON_GetObjectItem(cJSON_GetObjectItem(u, "repositories"), "nodes");
    st->stars = 0;
    cJSON *n = NULL;
    cJSON_ArrayForEach(n, nodes) {
        cJSON *s = cJSON_GetObjectItem(n, "stargazerCount");
        if (cJSON_IsNumber(s)) st->stars += s->valueint;
    }

    cJSON *coll = cJSON_GetObjectItem(u, "contributionsCollection");
    cJSON *cal  = coll ? cJSON_GetObjectItem(coll, "contributionCalendar") : NULL;
    if (cJSON_IsObject(cal)) {
        cJSON *tot = cJSON_GetObjectItem(cal, "totalContributions");
        st->contrib_total = cJSON_IsNumber(tot) ? tot->valueint : 0;

        // Keep raw counts while parsing: levels are relative to the year's max,
        // and a busy day can far exceed what a uint8_t would hold.
        static int raw[GH_CAL_WEEKS][7];
        static int flat[GH_CAL_WEEKS * 7];
        memset(raw, 0, sizeof(raw));
        int flat_n = 0, maxc = 0;

        cJSON *weeks = cJSON_GetObjectItem(cal, "weeks");
        int w = 0;
        cJSON *week = NULL;
        cJSON_ArrayForEach(week, weeks) {
            if (w >= GH_CAL_WEEKS) break;
            cJSON *day = NULL;
            cJSON_ArrayForEach(day, cJSON_GetObjectItem(week, "contributionDays")) {
                cJSON *cc = cJSON_GetObjectItem(day, "contributionCount");
                cJSON *wd = cJSON_GetObjectItem(day, "weekday");
                int count = cJSON_IsNumber(cc) ? cc->valueint : 0;
                int dow   = cJSON_IsNumber(wd) ? wd->valueint : 0;
                if (dow >= 0 && dow < 7) raw[w][dow] = count;
                if (count > maxc) maxc = count;
                if (flat_n < (int)(sizeof(flat) / sizeof(flat[0]))) flat[flat_n++] = count;
            }
            w++;
        }
        st->cal_weeks = w;

        memset(st->cal, 0, sizeof(st->cal));
        for (int i = 0; i < w; i++)
            for (int d = 0; d < 7; d++)
                st->cal[i][d] = (uint8_t)level_for(raw[i][d], maxc);

        st->contrib_today = flat_n ? flat[flat_n - 1] : 0;

        // Current streak: an empty "today" is normal mid-day and should not
        // break the run, so start from yesterday when today is still zero.
        int i = flat_n - 1;
        if (i >= 0 && flat[i] == 0) i--;
        int cur = 0;
        while (i >= 0 && flat[i] > 0) { cur++; i--; }
        st->streak_cur = cur;

        int run = 0, best = 0;
        for (int k = 0; k < flat_n; k++) {
            run = flat[k] > 0 ? run + 1 : 0;
            if (run > best) best = run;
        }
        st->streak_max = best;
    }

    st->authed = true;
    st->valid = true;
    st->err[0] = '\0';
    ret = ESP_OK;

done:
    cJSON_Delete(root);
    return ret;
}

// ------------------------------------------------------ REST (no token) ---
static esp_err_t fetch_rest(gh_stats_t *st, resp_t *r)
{
    char url[128];

    snprintf(url, sizeof(url), "https://api.github.com/users/%s", GITHUB_USER);
    r->len = 0; r->buf[0] = '\0';
    int status = gh_request(url, NULL, r);
    if (status != 200) {
        snprintf(st->err, sizeof(st->err),
                 status == 403 ? "rate limited - add a token" : "HTTP %d", status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(r->buf);
    if (!root) { snprintf(st->err, sizeof(st->err), "bad JSON"); return ESP_FAIL; }
    #define NUM(k) ({ cJSON *_x = cJSON_GetObjectItem(root, k); cJSON_IsNumber(_x) ? _x->valueint : 0; })
    st->followers = NUM("followers");
    st->following = NUM("following");
    st->repos     = NUM("public_repos");
    #undef NUM
    cJSON_Delete(root);

    // One page of repos is enough for a headline star count.
    snprintf(url, sizeof(url),
             "https://api.github.com/users/%s/repos?per_page=100&type=owner&sort=updated",
             GITHUB_USER);
    r->len = 0; r->buf[0] = '\0';
    if (gh_request(url, NULL, r) == 200) {
        cJSON *arr = cJSON_Parse(r->buf);
        cJSON *it = NULL;
        st->stars = 0;
        cJSON_ArrayForEach(it, arr) {
            cJSON *s = cJSON_GetObjectItem(it, "stargazers_count");
            cJSON *fork = cJSON_GetObjectItem(it, "fork");
            if (cJSON_IsTrue(fork)) continue;
            if (cJSON_IsNumber(s)) st->stars += s->valueint;
        }
        cJSON_Delete(arr);
    }

    st->authed = false;
    st->valid = true;
    st->open_prs = -1;          // not available without burning search quota
    st->contrib_total = -1;
    st->cal_weeks = 0;
    st->err[0] = '\0';
    return ESP_OK;
}

// ------------------------------------------------------------------ API ---
esp_err_t github_refresh(void)
{
    lock_init();

    resp_t r = { 0 };
    r.cap = RESP_MAX;
    r.buf = heap_caps_malloc(r.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r.buf) r.buf = malloc(r.cap);
    if (!r.buf) return ESP_ERR_NO_MEM;
    r.buf[0] = '\0';

    gh_stats_t st = { 0 };
    snprintf(st.user, sizeof(st.user), "%s", GITHUB_USER);

    esp_err_t ret;
    if (sizeof(GITHUB_TOKEN) > 1) {
        char *body = malloc(strlen(GQL) + 128);
        snprintf(body, strlen(GQL) + 128,
                 "{\"query\":\"%s\",\"variables\":{\"l\":\"%s\"}}", GQL, GITHUB_USER);
        int status = gh_request("https://api.github.com/graphql", body, &r);
        free(body);

        if (status == 200) {
            ret = parse_graphql(r.buf, &st);
        } else {
            snprintf(st.err, sizeof(st.err),
                     status == 401 ? "bad token" : "HTTP %d", status);
            ret = ESP_FAIL;
        }
    } else {
        ret = fetch_rest(&st, &r);
    }

    free(r.buf);
    st.fetched_at = (int64_t)time(NULL);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ret == ESP_OK) {
        s_stats = st;
    } else {
        // Keep the last good numbers on screen, just surface the error.
        snprintf(s_stats.err, sizeof(s_stats.err), "%s", st.err);
        snprintf(s_stats.user, sizeof(s_stats.user), "%s", st.user);
        ESP_LOGW(TAG, "refresh failed: %s", st.err);
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void github_get(gh_stats_t *out)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_lock);
}
