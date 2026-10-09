#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *AUTH_LOGIN = "https://auth.insigniastats.live/api/auth/login";
static const char *DEVICE_START = "https://xb.live/api/xbl/device/start";
static const char *DEVICE_POLL = "https://xb.live/api/xbl/device/poll";
static const char *PROVISION = "https://online.xb.live/api/v1/provision";

static void set_err(char *err, size_t cap, const char *msg)
{
    if (!err || !cap) return;
    snprintf(err, cap, "%s", msg ? msg : "");
}

static const char *json_find(const char *body, const char *key)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = body ? strstr(body, pat) : NULL;
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') p++;
    return p;
}

static int json_str(const char *body, const char *key, char *out, size_t cap)
{
    const char *p = json_find(body, key);
    size_t n = 0;
    if (!p || *p != '"') return -1;
    p++;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return n ? 0 : -1;
}

static int json_hex16(const char *body, const char *key, uint8_t out[16])
{
    char hex[40];
    size_t i;
    if (json_str(body, key, hex, sizeof(hex)) != 0 || strlen(hex) != 32) return -1;
    for (i = 0; i < 16; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int json_xuid(const char *body, const char *key, uint64_t *out)
{
    char s[40];
    const char *p = json_find(body, key);
    if (!p) return -1;
    if (*p == '"') {
        if (json_str(body, key, s, sizeof(s)) != 0) return -1;
    } else {
        size_t n = 0;
        while (p[n] && p[n] != ',' && p[n] != '}' && n + 1 < sizeof(s)) {
            s[n] = p[n];
            n++;
        }
        s[n] = 0;
    }
    if (strlen(s) == 16)
        *out = strtoull(s, NULL, 16);
    else
        *out = strtoull(s, NULL, 10);
    return *out ? 0 : -1;
}

static void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; in && *in && o + 2 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) break;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

static const char *provision_url(void)
{
    const char *e = getenv("XBL_ONLINE_PROVISION_URL");
    return (e && e[0]) ? e : PROVISION;
}

static int install_from_provision(const char *body, const char *email, char *err, size_t err_cap)
{
    XblConsole console;
    XblAccount acct;
    char save[512], gt[XBL_GT_MAX], msg[160];
    const char *env;

    memset(&acct, 0, sizeof(acct));
    if (xbl_console_load(&console) != 0) {
        set_err(err, err_cap, "Could not create a console identity for this PC.");
        return -1;
    }
    if (json_str(body, "gamertag", gt, sizeof(gt)) != 0 || !gt[0]) {
        set_err(err, err_cap, "XB Online did not return a gamertag.");
        return -1;
    }
    snprintf(acct.gamertag, sizeof(acct.gamertag), "%s", gt);
    json_xuid(body, "xuid", &acct.xuid);
    if (!acct.xuid) json_xuid(body, "user_xuid", &acct.xuid);
    if (json_hex16(body, "user_client_master_key", acct.user_key) == 0)
        acct.has_user_key = 1;
    if (json_hex16(body, "machine_account_key", acct.machine_key) == 0)
        acct.has_machine_key = 1;
    if (email) snprintf(acct.email, sizeof(acct.email), "%s", email);
    acct.present = 1;

    env = getenv("XBL_ONLINE_SAVE_DIR");
    if (env && env[0]) snprintf(save, sizeof(save), "%s", env);
    else xbl_default_save_dir(save, sizeof(save));

    if (xbl_account_save(&acct) != 0 || xbl_disk_install(save, &acct, &console) != 0) {
        set_err(err, err_cap, "Signed in, but the account could not be written for the game.");
        return -1;
    }
    json_str(body, "message", msg, sizeof(msg));
    if (!msg[0]) snprintf(msg, sizeof(msg), "XB Online account ready: %s", acct.gamertag);
    xbl_status_set(msg);
    set_err(err, err_cap, msg);
    return 0;
}

static int provision(const char *email, const char *gamertag, const char *password,
                     int console_link, char *err, size_t err_cap)
{
    XblConsole c;
    char body[768], resp[4096], pw[80], mac[32];
    const char *url = provision_url();
    long status = 0;

    if (xbl_console_load(&c) != 0) {
        set_err(err, err_cap, "Could not read this PC's console identity.");
        return -1;
    }
    json_escape(password ? password : "", pw, sizeof(pw));
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             c.mac[0], c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5]);
    snprintf(body, sizeof(body),
             "{\"serial_number\":\"%s\",\"mac_address\":\"%s\","
             "\"online_key\":\"%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\","
             "\"hdd_key\":\"%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\","
             "\"email\":\"%s\",\"gamertag\":\"%s\"%s%s%s%s}",
             c.serial, mac,
             c.online_key[0], c.online_key[1], c.online_key[2], c.online_key[3],
             c.online_key[4], c.online_key[5], c.online_key[6], c.online_key[7],
             c.online_key[8], c.online_key[9], c.online_key[10], c.online_key[11],
             c.online_key[12], c.online_key[13], c.online_key[14], c.online_key[15],
             c.hdd_key[0], c.hdd_key[1], c.hdd_key[2], c.hdd_key[3],
             c.hdd_key[4], c.hdd_key[5], c.hdd_key[6], c.hdd_key[7],
             c.hdd_key[8], c.hdd_key[9], c.hdd_key[10], c.hdd_key[11],
             c.hdd_key[12], c.hdd_key[13], c.hdd_key[14], c.hdd_key[15],
             email, gamertag,
             password && password[0] ? ",\"password\":\"" : "",
             password && password[0] ? pw : "",
             password && password[0] ? "\"" : "",
             console_link ? ",\"console_link\":true" : "");

    if (xbl_http_post(url, body, resp, sizeof(resp), &status) != 0) {
        set_err(err, err_cap, "Could not reach XB Online.");
        return -1;
    }
    if (status != 200) {
        char m[160];
        if (json_str(resp, "message", m, sizeof(m)) != 0)
            snprintf(m, sizeof(m), "XB Online returned HTTP %ld.", status);
        set_err(err, err_cap, m);
        return -1;
    }
    return install_from_provision(resp, email, err, err_cap);
}

static int pull_account(const char *email, const char *gamertag, const char *password,
                        char *err, size_t err_cap)
{
    if (!gamertag || !gamertag[0] || strchr(gamertag, '@')) {
        set_err(err, err_cap, "This xb.live account has no gamertag yet.");
        return -1;
    }
    xbl_status_set("Pulling XB Online account...");
    if (provision(email, gamertag, password, 0, err, err_cap) == 0) return 0;
    return provision(email, gamertag, password, 1, err, err_cap);
}

int xbl_signin_password(const char *email, const char *password, char *err, size_t err_cap)
{
    char e_email[96], e_pass[160], body[320], resp[2048], user[48];
    long status = 0;
    if (!email || !email[0] || !password || !password[0]) {
        set_err(err, err_cap, "Enter the XB Online email and password you use on xb.live.");
        return -1;
    }
    json_escape(email, e_email, sizeof(e_email));
    json_escape(password, e_pass, sizeof(e_pass));
    snprintf(body, sizeof(body), "{\"email\":\"%s\",\"password\":\"%s\"}", e_email, e_pass);
    xbl_status_set("Signing in to xb.live...");
    if (xbl_http_post(AUTH_LOGIN, body, resp, sizeof(resp), &status) != 0) {
        set_err(err, err_cap, "Could not reach xb.live.");
        return -1;
    }
    if (status != 200 ||
        (!strstr(resp, "\"success\":true") && !strstr(resp, "\"success\": true"))) {
        char m[160];
        if (json_str(resp, "error", m, sizeof(m)) != 0 &&
            json_str(resp, "message", m, sizeof(m)) != 0)
            snprintf(m, sizeof(m), "Sign-in failed (HTTP %ld).", status);
        set_err(err, err_cap, m);
        xbl_status_set(m);
        return -1;
    }
    if (json_str(resp, "username", user, sizeof(user)) != 0)
        snprintf(user, sizeof(user), "%s", email);
    return pull_account(email, user, password, err, err_cap);
}

int xbl_signin_device_start(char *user_code, size_t code_cap, char *poll_token, size_t token_cap,
                            char *err, size_t err_cap)
{
    char resp[2048];
    long status = 0;
    if (user_code && code_cap) user_code[0] = 0;
    if (poll_token && token_cap) poll_token[0] = 0;
    if (xbl_http_post(DEVICE_START, "{}", resp, sizeof(resp), &status) != 0 || status != 200) {
        set_err(err, err_cap, "Could not start a QR sign-in. Check the connection and refresh.");
        return -1;
    }
    if (json_str(resp, "user_code", user_code, code_cap) != 0 ||
        json_str(resp, "poll_token", poll_token, token_cap) != 0) {
        set_err(err, err_cap, "xb.live did not return a sign-in code.");
        return -1;
    }
    set_err(err, err_cap, "Scan the code, then sign in on xb.live with your XB Online account.");
    return 0;
}

int xbl_signin_device_poll(const char *poll_token, char *err, size_t err_cap)
{
    char body[160], resp[2048], st[24], user[48], email[64], password[40];
    long status = 0;
    if (!poll_token || !poll_token[0]) return -1;
    snprintf(body, sizeof(body), "{\"poll_token\":\"%s\"}", poll_token);
    if (xbl_http_post(DEVICE_POLL, body, resp, sizeof(resp), &status) != 0) return 0;
    if (status != 200) return 0;
    if (json_str(resp, "status", st, sizeof(st)) != 0) return 0;
    if (strcmp(st, "pending") == 0) return 0;
    if (strcmp(st, "approved") != 0) {
        set_err(err, err_cap, "That QR code expired. Refresh the page.");
        return -1;
    }
    json_str(resp, "username", user, sizeof(user));
    json_str(resp, "email", email, sizeof(email));
    json_str(resp, "password", password, sizeof(password));
    if (!email[0]) snprintf(email, sizeof(email), "%s", user);
    if (pull_account(email, user, password, err, err_cap) != 0) return -1;
    return 1;
}
