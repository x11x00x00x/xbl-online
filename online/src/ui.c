#include "internal.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <xinput.h>
typedef SOCKET sock_t;
#define SOCK_BAD(s) ((s) == INVALID_SOCKET)
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD(s) ((s) < 0)
#define CLOSESOCK close
#define INVALID_SOCKET (-1)
#endif

static char g_status[200] = "XB Online is not signed in.";
static char g_url[64] = "";
static char g_code[16];
static char g_token[80];
static char g_page_err[200];
static int g_started;

#ifdef _WIN32
static CRITICAL_SECTION g_mu;
static void mu_init(void) { InitializeCriticalSection(&g_mu); }
static void mu_lock(void) { EnterCriticalSection(&g_mu); }
static void mu_unlock(void) { LeaveCriticalSection(&g_mu); }
#else
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static void mu_init(void) {}
static void mu_lock(void) { pthread_mutex_lock(&g_mu); }
static void mu_unlock(void) { pthread_mutex_unlock(&g_mu); }
#endif

void xbl_status_set(const char *msg)
{
    mu_lock();
    snprintf(g_status, sizeof(g_status), "%s", msg ? msg : "");
    mu_unlock();
    fprintf(stderr, "[xbl] %s\n", msg ? msg : "");
}

const char *xbl_online_status(void) { return g_status; }
const char *xbl_online_signin_url(void) { return g_url; }

const char *xbl_online_gamertag(void)
{
    static XblAccount a;
    if (xbl_account_load(&a) != 0) return "";
    return a.gamertag;
}

int xbl_online_network(void) { return 1; }

static void open_browser(const char *url)
{
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "open \"%s\"", url);
    system(cmd);
#else
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" >/dev/null 2>&1", url);
    system(cmd);
#endif
}

static int form_value(const char *body, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p;
    size_t n = 0;
    snprintf(pat, sizeof(pat), "%s=", key);
    p = body ? strstr(body, pat) : NULL;
    if (!p) return -1;
    p += strlen(pat);
    while (*p && *p != '&' && n + 1 < cap) {
        if (*p == '%' && p[1] && p[2]) {
            unsigned v;
            if (sscanf(p + 1, "%2x", &v) == 1) {
                out[n++] = (char)v;
                p += 3;
                continue;
            }
        }
        if (*p == '+') out[n++] = ' ';
        else out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return 0;
}

static void html_qr(char *dst, size_t cap, const char *text)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    int size, y, x;
    size_t used = 0;
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, 8,
                              qrcodegen_Mask_AUTO, true)) {
        snprintf(dst, cap, "<p>QR unavailable</p>");
        return;
    }
    size = qrcodegen_getSize(qr);
    used += (size_t)snprintf(dst + used, cap - used,
                             "<div style=\"display:inline-block;background:#fff;padding:12px\">");
    for (y = 0; y < size && used + 64 < cap; y++) {
        used += (size_t)snprintf(dst + used, cap - used, "<div style=\"height:6px;font-size:0;line-height:0\">");
        for (x = 0; x < size && used + 80 < cap; x++) {
            int dark = qrcodegen_getModule(qr, x, y);
            used += (size_t)snprintf(dst + used, cap - used,
                                     "<i style=\"display:inline-block;width:6px;height:6px;background:%s\"></i>",
                                     dark ? "#111" : "#fff");
        }
        used += (size_t)snprintf(dst + used, cap - used, "</div>");
    }
    snprintf(dst + used, cap - used, "</div>");
}

static void send_all(sock_t s, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int w = (int)send(s, buf + off, (int)(n - off), 0);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

static void reply(sock_t s, const char *status, const char *body)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.0 %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %u\r\nConnection: close\r\n\r\n",
                     status, (unsigned)strlen(body));
    send_all(s, head, (size_t)n);
    send_all(s, body, strlen(body));
}

static void page(sock_t s)
{
    XblAccount acct;
    char qr[48000];
    char body[56000];
    char link[128];
    char err[200];
    int signed_in;

    memset(&acct, 0, sizeof(acct));
    signed_in = xbl_account_load(&acct) == 0 && acct.gamertag[0];
    err[0] = 0;
    mu_lock();
    snprintf(err, sizeof(err), "%s", g_page_err);
    if (!signed_in && !g_code[0]) {
        mu_unlock();
        if (xbl_signin_device_start(g_code, sizeof(g_code), g_token, sizeof(g_token),
                                    err, sizeof(err)) != 0) {
            mu_lock();
            snprintf(g_page_err, sizeof(g_page_err), "%s", err);
            mu_unlock();
        }
        mu_lock();
    }
    snprintf(link, sizeof(link), "https://xb.live/link?code=%s", g_code);
    mu_unlock();

    if (signed_in) {
        snprintf(body, sizeof(body),
                 "<!doctype html><meta charset=utf-8><title>XB Online</title>"
                 "<body style=\"font:16px sans-serif;background:#10140f;color:#e8f0e4;margin:40px\">"
                 "<h1>XB Online</h1><p>Signed in as <b>%s</b>.</p>"
                 "<p>Back out of the game's account list and open it again. "
                 "The gamertag is already on the disk the game reads.</p>"
                 "<form method=post action=/signout><button>Sign out</button></form></body>",
                 acct.gamertag);
        reply(s, "200 OK", body);
        return;
    }

    html_qr(qr, sizeof(qr), link);
    snprintf(body, sizeof(body),
             "<!doctype html><meta charset=utf-8><title>XB Online</title>"
             "<body style=\"font:16px sans-serif;background:#10140f;color:#e8f0e4;margin:40px\">"
             "<h1>Sign in to XB Online</h1>"
             "<p>Sign in with your XB Online account on xb.live.</p>"
             "<p>Scan this, or open <a style=\"color:#9c6\" href=\"%s\">%s</a> and enter <b>%s</b>.</p>"
             "%s"
             "<p style=\"color:#c96\">%s</p>"
             "<h2>Or type your XB Online login</h2>"
             "<form method=post action=/password>"
             "<p>Email<br><input name=email size=32></p>"
             "<p>Password<br><input name=password type=password size=32></p>"
             "<button>Sign in and download the account</button></form>"
             "<p><a style=\"color:#9c6\" href=\"/?refresh=1\">New code</a></p></body>",
             link, link, g_code[0] ? g_code : "----", qr, err);
    reply(s, "200 OK", body);
}

static void handle(sock_t s, char *req, int refresh)
{
    char *body = strstr(req, "\r\n\r\n");
    char err[200];
    if (body) body += 4;
    if (strncmp(req, "POST /signout", 13) == 0) {
        XblConsole c;
        XblAccount empty;
        char save[512];
        xbl_account_clear();
        memset(&empty, 0, sizeof(empty));
        if (xbl_console_load(&c) == 0) {
            const char *env = getenv("XBL_ONLINE_SAVE_DIR");
            if (env && env[0]) snprintf(save, sizeof(save), "%s", env);
            else xbl_default_save_dir(save, sizeof(save));
            xbl_disk_install(save, &empty, &c);
        }
        mu_lock();
        g_code[0] = 0;
        g_token[0] = 0;
        snprintf(g_page_err, sizeof(g_page_err), "Signed out.");
        mu_unlock();
        xbl_status_set("Signed out.");
        page(s);
        return;
    }
    if (strncmp(req, "POST /password", 14) == 0) {
        char email[64], password[40];
        form_value(body, "email", email, sizeof(email));
        form_value(body, "password", password, sizeof(password));
        if (xbl_signin_password(email, password, err, sizeof(err)) != 0) {
            mu_lock();
            snprintf(g_page_err, sizeof(g_page_err), "%s", err);
            mu_unlock();
        } else {
            mu_lock();
            g_page_err[0] = 0;
            mu_unlock();
        }
        page(s);
        return;
    }
    if (refresh) {
        mu_lock();
        g_code[0] = 0;
        g_token[0] = 0;
        mu_unlock();
    }
    page(s);
}

static void poll_once(void)
{
    char token[80], err[200];
    int rc;
    mu_lock();
    snprintf(token, sizeof(token), "%s", g_token);
    mu_unlock();
    if (!token[0]) return;
    if (xbl_online_gamertag()[0]) return;
    rc = xbl_signin_device_poll(token, err, sizeof(err));
    if (rc < 0) {
        mu_lock();
        g_code[0] = 0;
        g_token[0] = 0;
        snprintf(g_page_err, sizeof(g_page_err), "%s", err);
        mu_unlock();
    }
}

#ifdef _WIN32
static DWORD WINAPI ui_thread(LPVOID arg)
#else
static void *ui_thread(void *arg)
#endif
{
    sock_t srv;
    struct sockaddr_in addr;
    int port;
    (void)arg;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (SOCK_BAD(srv)) return 0;
    {
        int yes = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);
    for (port = 4765; port < 4780; port++) {
        addr.sin_port = htons((unsigned short)port);
        if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0) break;
    }
    if (port >= 4780 || listen(srv, 4) != 0) {
        CLOSESOCK(srv);
        xbl_status_set("Could not open the sign-in page.");
        return 0;
    }
    snprintf(g_url, sizeof(g_url), "http://127.0.0.1:%d/", port);
    fprintf(stderr, "[xbl] Sign in at %s  (F10 or both stick clicks opens it)\n", g_url);
    if (!xbl_online_gamertag()[0]) open_browser(g_url);

    for (;;) {
        fd_set fds;
        xbl_keys_publish();
        struct timeval tv;
        sock_t c;
        FD_ZERO(&fds);
        FD_SET(srv, &fds);
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        if (select((int)srv + 1, &fds, NULL, NULL, &tv) > 0) {
            char req[4096];
            int n;
            c = accept(srv, NULL, NULL);
            if (SOCK_BAD(c)) continue;
            n = (int)recv(c, req, sizeof(req) - 1, 0);
            if (n < 0) n = 0;
            req[n] = 0;
            handle(c, req, strstr(req, "GET /?refresh=1") != NULL);
            CLOSESOCK(c);
        } else {
            poll_once();
        }
#ifdef _WIN32
        {
            XINPUT_STATE st;
            SHORT f10 = GetAsyncKeyState(VK_F10);
            int chord = 0;
            if (f10 & 1) chord = 1;
            if (XInputGetState(0, &st) == 0) {
                WORD b = st.Gamepad.wButtons;
                if ((b & XINPUT_GAMEPAD_LEFT_THUMB) && (b & XINPUT_GAMEPAD_RIGHT_THUMB))
                    chord = 1;
            }
            if (chord) open_browser(g_url);
        }
#endif
    }
}

void xbl_ui_start(void)
{
    if (g_started) return;
    g_started = 1;
    mu_init();
#ifdef _WIN32
    CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
#else
    {
        pthread_t th;
        pthread_create(&th, NULL, ui_thread, NULL);
        pthread_detach(th);
    }
#endif
}
