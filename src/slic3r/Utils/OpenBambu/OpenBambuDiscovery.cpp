#include "OpenBambuDiscovery.hpp"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <poll.h>
#  define closesocket close
#endif

#include <cstring>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <map>

namespace OpenBambu {

static constexpr const char *SSDP_MULTICAST_ADDR = "239.255.255.250";
static constexpr uint16_t SSDP_PORTS[] = {2021, 1990};
static constexpr int NUM_SSDP_PORTS = 2;
static constexpr const char *BAMBU_SEARCH_TARGET = "urn:bambulab-com:device:3dprinter:1";

// M-SEARCH probe template
static constexpr const char *MSEARCH_TEMPLATE =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:%d\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 3\r\n"
    "ST: urn:bambulab-com:device:3dprinter:1\r\n"
    "\r\n";

// Parse HTTP-style headers from an SSDP message.
// Returns a map of header-name (lowercased) -> value.
static std::map<std::string, std::string> parse_headers(const std::string &msg)
{
    std::map<std::string, std::string> headers;
    std::istringstream iss(msg);
    std::string line;

    // Skip the first line (NOTIFY/HTTP status line)
    if (!std::getline(iss, line))
        return headers;

    while (std::getline(iss, line)) {
        // Remove trailing \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            break;

        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;

        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);

        // Trim leading whitespace from value
        auto start = val.find_first_not_of(" \t");
        if (start != std::string::npos)
            val = val.substr(start);

        // Lowercase the key for case-insensitive matching
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        headers[key] = val;
    }
    return headers;
}

Discovery::Discovery()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

Discovery::~Discovery()
{
    stop();
}

void Discovery::set_callback(OnDeviceFound cb)
{
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_callback = std::move(cb);
}

bool Discovery::start()
{
    if (m_running.load())
        return true;

    m_stop_requested.store(false);
    m_listener_thread = std::thread(&Discovery::listener_thread_func, this);
    return true;
}

void Discovery::stop()
{
    m_stop_requested.store(true);
    if (m_listener_thread.joinable())
        m_listener_thread.join();
}

static Discovery::socket_t create_multicast_socket(uint16_t port)
{
    auto sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == Discovery::INVALID_SOCK)
        return Discovery::INVALID_SOCK;

    // Allow multiple listeners on the same port
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char *)&reuse, sizeof(reuse));
#endif

    // Bind to the multicast port
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(port);

    if (bind(sock, (sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        closesocket(sock);
        return Discovery::INVALID_SOCK;
    }

    // Join the multicast group on ALL interfaces (not just the default)
    // This is important on multi-homed systems (WSL, VPN, Hyper-V)
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_MULTICAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    fprintf(stderr, "[DEBUG] Binding multicast on port %d, joining group %s\n", port, SSDP_MULTICAST_ADDR);

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "[DEBUG] Failed to join multicast group on port %d (error %d)\n", port,
#ifdef _WIN32
            WSAGetLastError()
#else
            errno
#endif
        );
        closesocket(sock);
        return Discovery::INVALID_SOCK;
    }
    fprintf(stderr, "[DEBUG] Successfully joined multicast, socket ready on port %d\n", port);

    // Set receive timeout (1 second) so we can check stop_requested periodically
#ifdef _WIN32
    DWORD timeout_ms = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    return sock;
}

void Discovery::send_msearch(socket_t sock, uint16_t port)
{
    char buf[512];
    snprintf(buf, sizeof(buf), MSEARCH_TEMPLATE, port);

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(SSDP_MULTICAST_ADDR);
    dest.sin_port = htons(port);

    int sent = sendto(sock, buf, (int)strlen(buf), 0, (sockaddr *)&dest, sizeof(dest));
    (void)sent; // silence unused warning in release
}

void Discovery::send_msearch_broadcast(socket_t sock, uint16_t port)
{
    char buf[512];
    snprintf(buf, sizeof(buf), MSEARCH_TEMPLATE, port);

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr("255.255.255.255");
    dest.sin_port = htons(port);

    int sent = sendto(sock, buf, (int)strlen(buf), 0, (sockaddr *)&dest, sizeof(dest));
    (void)sent;
}

std::string Discovery::parse_ssdp_message(const std::string &message, const std::string &source_ip)
{
    auto headers = parse_headers(message);

    // Check if this is a Bambu printer — look for the NT or ST header
    bool is_bambu = false;
    if (headers.count("nt") && headers["nt"].find("bambulab") != std::string::npos)
        is_bambu = true;
    if (headers.count("st") && headers["st"].find("bambulab") != std::string::npos)
        is_bambu = true;
    // Also check for Bambu custom headers as a fallback
    if (headers.count("devmodel.bambu.com"))
        is_bambu = true;

    if (!is_bambu)
        return {};

    // Extract fields from Bambu custom headers
    std::string dev_model   = headers.count("devmodel.bambu.com")   ? headers["devmodel.bambu.com"]   : "";
    std::string dev_name    = headers.count("devname.bambu.com")    ? headers["devname.bambu.com"]    : "";
    std::string dev_signal  = headers.count("devsignal.bambu.com")  ? headers["devsignal.bambu.com"]  : "";
    std::string dev_connect = headers.count("devconnect.bambu.com") ? headers["devconnect.bambu.com"] : "lan";
    std::string dev_bind    = headers.count("devbind.bambu.com")    ? headers["devbind.bambu.com"]    : "free";
    std::string dev_seclink = headers.count("devseclink.bambu.com") ? headers["devseclink.bambu.com"] : "";
    std::string dev_version = headers.count("devversion.bambu.com") ? headers["devversion.bambu.com"] : "";

    // Serial number is in the USN header, format: "USN: SERIAL_NUMBER"
    std::string dev_id;
    if (headers.count("usn")) {
        dev_id = headers["usn"];
        // Strip any "uuid:" prefix if present
        if (dev_id.substr(0, 5) == "uuid:")
            dev_id = dev_id.substr(5);
        // Strip any "::..." suffix
        auto pos = dev_id.find("::");
        if (pos != std::string::npos)
            dev_id = dev_id.substr(0, pos);
    }

    if (dev_id.empty() || dev_model.empty())
        return {};

    // Build the JSON that OrcaSlicer's DeviceManager::on_machine_alive() expects
    std::ostringstream json;
    json << "{";
    json << "\"dev_name\":\""    << dev_name    << "\",";
    json << "\"dev_id\":\""      << dev_id      << "\",";
    json << "\"dev_ip\":\""      << source_ip   << "\",";
    json << "\"dev_type\":\""    << dev_model    << "\",";
    json << "\"dev_signal\":\""  << dev_signal   << "\",";
    json << "\"connect_type\":\"" << dev_connect << "\",";
    json << "\"bind_state\":\""  << dev_bind     << "\",";
    json << "\"sec_link\":\""    << dev_seclink  << "\",";
    json << "\"ssdp_version\":\"" << dev_version << "\"";
    json << "}";

    return json.str();
}

void Discovery::listener_thread_func()
{
    m_running.store(true);

    // Use a single ephemeral UDP socket for both M-SEARCH and receiving responses.
    // Multicast sockets on fixed ports had issues with Windows firewall silently
    // blocking inbound traffic. The ephemeral approach matches what works in practice.
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCK) {
        m_running.store(false);
        return;
    }

    // Bind to any available port on the default route interface
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = 0;
    bind(sock, (sockaddr *)&bind_addr, sizeof(bind_addr));

    // Enable broadcast for wider compatibility
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&bcast, sizeof(bcast));

    // Set 2 second receive timeout
#ifdef _WIN32
    DWORD timeout_ms = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // Send initial M-SEARCH probes (multicast + broadcast for reliability)
    for (int i = 0; i < NUM_SSDP_PORTS; ++i) {
        send_msearch(sock, SSDP_PORTS[i]);
        // Also broadcast
        send_msearch_broadcast(sock, SSDP_PORTS[i]);
    }

    auto last_msearch = std::chrono::steady_clock::now();
    char buf[4096];

    while (!m_stop_requested.load()) {
        sockaddr_in sender{};
        int sender_len = sizeof(sender);

        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (sockaddr *)&sender, &sender_len);

        if (n > 0) {
            buf[n] = '\0';
            std::string source_ip = inet_ntoa(sender.sin_addr);
            std::string json = parse_ssdp_message(std::string(buf, n), source_ip);

            if (!json.empty()) {
                // Dedup: extract dev_id from JSON for rate-limiting
                bool should_fire = true;
                {
                    // Quick extract of dev_id from the JSON
                    std::string dev_id;
                    auto pos = json.find("\"dev_id\":\"");
                    if (pos != std::string::npos) {
                        pos += 10;
                        auto end = json.find("\"", pos);
                        if (end != std::string::npos)
                            dev_id = json.substr(pos, end - pos);
                    }

                    if (!dev_id.empty()) {
                        std::lock_guard<std::mutex> lock(m_dedup_mutex);
                        auto now_dedup = std::chrono::steady_clock::now();
                        auto it = m_last_seen.find(dev_id);
                        if (it != m_last_seen.end()) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                now_dedup - it->second).count();
                            if (elapsed < DEDUP_INTERVAL_SEC)
                                should_fire = false;
                        }
                        if (should_fire)
                            m_last_seen[dev_id] = now_dedup;
                    }
                }

                if (should_fire) {
                    std::lock_guard<std::mutex> lock(m_callback_mutex);
                    if (m_callback)
                        m_callback(json);
                }
            }
        }

        // Re-send M-SEARCH every 30 seconds
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_msearch).count() >= 30) {
            for (int i = 0; i < NUM_SSDP_PORTS; ++i) {
                send_msearch(sock, SSDP_PORTS[i]);
                send_msearch_broadcast(sock, SSDP_PORTS[i]);
            }
            last_msearch = now;
        }
    }

    closesocket(sock);

    m_running.store(false);
}

} // namespace OpenBambu
