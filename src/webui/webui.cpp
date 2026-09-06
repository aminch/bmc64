//
// webui.cpp
//
// Minimal HTTP/1.1 server for the BMC64 LAN web UI. Runs as a single
// Circle scheduler task on the kernel/networking core (core 0). It never
// runs on the emulator core; long I/O loops yield so the cooperative
// scheduler keeps servicing the network stack and other tasks.
//
// Endpoints: GET static assets, GET /api/status, POST /api/reboot,
// GET /api/volumes, GET /api/fs/list, GET /api/fs/download,
// POST /api/fs/upload, POST /api/fs/delete, POST /api/webui/disable.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "webui.h"

#if defined(RASPI_C64) || defined(RASPI_C128)

#include "webui_assets.h"
#include "webui_fs.h"
#include "webui_http.h"

#include <circle/bcmpropertytags.h>
#include <circle/logger.h>
#include <circle/machineinfo.h>
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/startup.h>
#include <circle/timer.h>
#include <circle/types.h>

#include <stdio.h>
#include <string.h>

// circle_get_network_status() / circle_get_network_ip_address() are the
// C bridge already implemented in src/vice_network.cpp.
extern "C" int circle_get_network_status(void);
extern "C" int circle_get_network_ip_address(char *address,
                                             unsigned int address_size);
// BMC64 version string; single source of truth is menu.c's VERSION_STRING.
extern "C" const char *bmc64_version_string(void);

#define WEBUI_LOG              "webui"
#define WEBUI_PORT             80
#define WEBUI_ALT_PORT         8080
#define WEBUI_TASK_STACK       0x10000
#define WEBUI_RECV_TIMEOUT_US  8000000
#define WEBUI_REQUEST_MAX      4096
#define WEBUI_LISTEN_BACKLOG   8

// CNetSubSystem is created with this name in src/viceapp.cpp.
#define WEBUI_HOSTNAME         "bmc64"

#if defined(RASPI_C64)
static const char *const kMachineName = "C64";
#else
static const char *const kMachineName = "C128";
#endif

namespace {

// Keep in sync with the CIRCLE_NETWORK_* enum in third_party/common/circle.h.
const char *NetStatusText(int status) {
  switch (status) {
  case 0:  return "disabled";
  case 1:  return "Ethernet initializing";
  case 2:  return "waiting for DHCP";
  case 3:  return "connected (Ethernet)";
  case 4:  return "Ethernet unavailable";
  case 5:  return "Ethernet init failed";
  case 6:  return "Wi-Fi config missing";
  case 7:  return "Wi-Fi device initializing";
  case 8:  return "Wi-Fi device init failed";
  case 9:  return "Wi-Fi network init failed";
  case 10: return "Wi-Fi WPA initializing";
  case 11: return "Wi-Fi WPA init failed";
  case 12: return "Wi-Fi connecting";
  case 13: return "connected (Wi-Fi)";
  case 14: return "Wi-Fi connection timeout";
  case 15: return "Wi-Fi unavailable";
  case 16: return "Wi-Fi device not initialized";
  case 17: return "Wi-Fi unsupported";
  case 18: return "Wi-Fi firmware missing";
  default: return "unknown";
  }
}

using webhttp::SendResponse;
using webhttp::SendText;

// Case-insensitive test whether s begins with the (already lowercase) prefix.
boolean CiPrefix(const char *s, const char *lower_prefix) {
  for (; *lower_prefix != '\0'; s++, lower_prefix++) {
    char c = *s;
    if (c >= 'A' && c <= 'Z') {
      c = (char) (c + 32);
    }
    if (c != *lower_prefix) {
      return FALSE;
    }
  }
  return TRUE;
}

// Value of the Content-Length header in a raw request, or -1 if absent.
long ParseContentLength(const char *request) {
  const char *p = request;
  while (*p != '\0') {
    if (CiPrefix(p, "content-length:")) {
      p += 15;
      while (*p == ' ' || *p == '\t') {
        p++;
      }
      long value = 0;
      boolean any = FALSE;
      while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
        any = TRUE;
      }
      return any ? value : -1;
    }
    const char *nl = strstr(p, "\r\n");
    if (nl == 0) {
      break;
    }
    p = nl + 2;
  }
  return -1;
}

const struct webui_asset *FindAsset(const char *path) {
  for (unsigned i = 0; i < g_webui_asset_count; i++) {
    if (strcmp(g_webui_assets[i].path, path) == 0) {
      return &g_webui_assets[i];
    }
  }
  return 0;
}

// SoC temperature as a JSON number ("47.2") into out, or "null" if the
// VideoCore mailbox query fails (e.g. QEMU).
void SocTempField(char *out, unsigned out_size) {
  CBcmPropertyTags tags;
  TPropertyTagTemperature tag;
  tag.nTemperatureId = TEMPERATURE_ID;
  if (tags.GetTag(PROPTAG_GET_TEMPERATURE, &tag, sizeof(tag), sizeof(u32)) &&
      tag.nValue > 0) {
    snprintf(out, out_size, "%u.%01u", tag.nValue / 1000,
             (tag.nValue % 1000) / 100);
  } else {
    strncpy(out, "null", out_size);
    out[out_size - 1] = '\0';
  }
}

// Raspberry Pi firmware "get_throttled" bitmask, forwarded raw and
// decoded client side. These are independent conditions: bit 0/16
// under-voltage (a 5V supply problem), bit 2/18 the ARM clock actually
// being throttled (from under-voltage or the 85C hard thermal limit),
// bit 1/17 the turbo frequency being capped, bit 3/19 the Pi 3B+
// "soft" temp limit (config.txt temp_soft_limit, default 60C): a
// partial throttle easing the ARM clock from 1.4GHz to 1.2GHz. Low
// bits are "now", bits 16-19 "since boot".
// "null" if the mailbox query is unavailable (e.g. QEMU).
void ThrottledField(char *out, unsigned out_size) {
  CBcmPropertyTags tags;
  TPropertyTagSimple tag;
  if (tags.GetTag(PROPTAG_GET_THROTTLED, &tag, sizeof(tag))) {
    snprintf(out, out_size, "%lu", (unsigned long) tag.nValue);
  } else {
    strncpy(out, "null", out_size);
    out[out_size - 1] = '\0';
  }
}

void HandleStatus(CSocket *socket) {
  char ip[32];
  boolean have_ip = circle_get_network_ip_address(ip, sizeof(ip)) != 0;
  int net_status = circle_get_network_status();
  unsigned uptime = CTimer::Get()->GetUptime();
  const char *model = CMachineInfo::Get()->GetMachineName();

  char soc_temp[16];
  char throttled[16];
  SocTempField(soc_temp, sizeof(soc_temp));
  ThrottledField(throttled, sizeof(throttled));

  char body[512];
  int length = snprintf(
      body, sizeof(body),
      "{\"hostname\":\"%s\",\"version\":\"%s\",\"machine\":\"%s\","
      "\"model\":\"%s\",\"ip\":\"%s\",\"net_status\":%d,\"net_text\":\"%s\","
      "\"uptime_secs\":%u,\"soc_temp_c\":%s,\"throttled\":%s}",
      WEBUI_HOSTNAME, bmc64_version_string(), kMachineName,
      model != 0 ? model : "",
      have_ip ? ip : "", net_status, NetStatusText(net_status), uptime,
      soc_temp, throttled);
  if (length < 0 || (unsigned) length >= sizeof(body)) {
    SendText(socket, 500, "Internal Server Error", "status encode error\n");
    return;
  }
  SendResponse(socket, 200, "OK", "application/json", body, (unsigned) length);
}

// Server lifecycle flags. s_started guards against a second task;
// g_webui_stop is set by POST /api/webui/disable to end the accept loop.
boolean s_started = FALSE;
boolean g_webui_stop = FALSE;

// Returns TRUE if a reboot was requested (caller must not touch the
// socket afterwards; reboot() does not return).
boolean HandleConnection(CSocket *socket) {
  socket->SetOptionReceiveTimeout(WEBUI_RECV_TIMEOUT_US);

  char request[WEBUI_REQUEST_MAX];
  unsigned length = 0;
  for (;;) {
    if (length >= sizeof(request) - 1) {
      SendText(socket, 431, "Request Header Fields Too Large",
               "request too large\n");
      return FALSE;
    }
    int n = socket->Receive(request + length, sizeof(request) - 1 - length, 0);
    if (n <= 0) {
      return FALSE;  // client closed, timed out, or error
    }
    length += (unsigned) n;
    request[length] = '\0';
    if (strstr(request, "\r\n\r\n") != 0) {
      break;
    }
  }

  // Body bytes that arrived in the same read(s) as the headers, plus the
  // declared body length (for streamed uploads).
  const unsigned char *body =
      (const unsigned char *) (strstr(request, "\r\n\r\n") + 4);
  unsigned body_prefetched = (unsigned) ((const unsigned char *) (request + length) - body);
  long content_length = ParseContentLength(request);

  char method[8];
  char target[512];
  if (sscanf(request, "%7s %511s", method, target) != 2) {
    SendText(socket, 400, "Bad Request", "bad request line\n");
    return FALSE;
  }

  const char *query = "";
  char *question = strchr(target, '?');
  if (question != 0) {
    *question = '\0';
    query = question + 1;
  }

  boolean is_get = strcmp(method, "GET") == 0;
  boolean is_post = strcmp(method, "POST") == 0;

  if (is_get && strcmp(target, "/api/status") == 0) {
    HandleStatus(socket);
    return FALSE;
  }

  if (is_get && strcmp(target, "/api/volumes") == 0) {
    WebUiFsVolumes(socket);
    return FALSE;
  }

  if (is_get && strcmp(target, "/api/fs/list") == 0) {
    WebUiFsList(socket, query);
    return FALSE;
  }

  if (is_get && strcmp(target, "/api/fs/download") == 0) {
    WebUiFsDownload(socket, query);
    return FALSE;
  }

  if (is_post && strcmp(target, "/api/fs/upload") == 0) {
    WebUiFsUpload(socket, query, body, body_prefetched, content_length);
    return FALSE;
  }

  if (is_post && strcmp(target, "/api/fs/delete") == 0) {
    WebUiFsDelete(socket, query);
    return FALSE;
  }

  if (is_post && strcmp(target, "/api/webui/disable") == 0) {
    const char *ok = "{\"ok\":true}";
    SendResponse(socket, 200, "OK", "application/json", ok,
                 (unsigned) strlen(ok));
    CLogger::Get()->Write(WEBUI_LOG, LogNotice,
                          "Web UI stop requested via web UI");
    g_webui_stop = TRUE;
    return FALSE;
  }

  if (is_post && strcmp(target, "/api/reboot") == 0) {
    const char *ok = "{\"ok\":true}";
    SendResponse(socket, 202, "Accepted", "application/json", ok,
                 (unsigned) strlen(ok));
    CLogger::Get()->Write(WEBUI_LOG, LogNotice,
                          "Reboot requested via web UI");
    CScheduler::Get()->MsSleep(250);  // give the socket time to flush
    reboot();                         // does not return
    return TRUE;
  }

  if (is_get) {
    const struct webui_asset *asset = FindAsset(target);
    if (asset != 0) {
      SendResponse(socket, 200, "OK", asset->content_type, asset->data,
                   asset->length);
      return FALSE;
    }
  }

  if (!is_get && !is_post) {
    SendText(socket, 405, "Method Not Allowed", "method not allowed\n");
    return FALSE;
  }

  SendText(socket, 404, "Not Found", "not found\n");
  return FALSE;
}

class CWebUiTask : public CTask {
public:
  explicit CWebUiTask(CNetSubSystem *network)
      : CTask(WEBUI_TASK_STACK), m_network(network) {
    SetName("webui");
  }

  void Run(void) override {
    while (!m_network->IsRunning()) {
      CScheduler::Get()->Sleep(1);
    }

    CSocket listener(m_network, IPPROTO_TCP);
    u16 port = WEBUI_PORT;
    if (listener.Bind(port) < 0) {
      port = WEBUI_ALT_PORT;
      if (listener.Bind(port) < 0) {
        CLogger::Get()->Write(WEBUI_LOG, LogError,
                              "Cannot bind port %u or %u", (unsigned) WEBUI_PORT,
                              (unsigned) WEBUI_ALT_PORT);
        return;
      }
    }

    if (listener.Listen(WEBUI_LISTEN_BACKLOG) < 0) {
      CLogger::Get()->Write(WEBUI_LOG, LogError, "Cannot listen on port %u",
                            (unsigned) port);
      return;
    }

    CLogger::Get()->Write(WEBUI_LOG, LogNotice,
                          "Web UI listening on port %u", (unsigned) port);

    for (;;) {
      CIPAddress client_ip;
      u16 client_port;
      CSocket *connection = listener.Accept(&client_ip, &client_port);
      if (connection == 0) {
        CScheduler::Get()->MsSleep(50);
        continue;
      }
      HandleConnection(connection);
      delete connection;
      if (g_webui_stop) {
        break;
      }
      CScheduler::Get()->Yield();
    }

    // listener goes out of scope here, freeing the port.
    CLogger::Get()->Write(WEBUI_LOG, LogNotice, "Web UI stopped");
    s_started = FALSE;
  }

private:
  CNetSubSystem *m_network;
};

}  // namespace

void WebUiStart(CNetSubSystem *network) {
  if (s_started || network == 0) {
    return;
  }
  s_started = TRUE;
  new CWebUiTask(network);  // registers itself with the scheduler
}

#else  // no Circle networking on this machine build

void WebUiStart(CNetSubSystem *) {}

#endif
