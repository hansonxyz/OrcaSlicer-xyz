#include "OpenBambuDiscovery.hpp"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  define closesocket close
#endif

#include <cstring>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <map>
#include <vector>
#include <boost/log/trivial.hpp>

namespace OpenBambu {

static constexpr const char *SSDP_MULTICAST_ADDR = "239.255.255.250";
static constexpr uint16_t SSDP_PORTS[] = {2021, 1990};
static constexpr int NUM_SSDP_PORTS = 2;

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

// Enumerate all active IPv4 addresses on this machine (one per interface).
// Skips loopback. Used to create per-interface sockets so SSDP probes go
// out every NIC with the correct source IP.
static std::vector<in_addr> get_local_ipv4_addresses()
{
    std::vector<in_addr> result;

#ifdef _WIN32
    ULONG bufSize = 15000;
    std::vector<BYTE> buf(bufSize);
    auto *addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());

    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, addrs, &bufSize);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufSize);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, addrs, &bufSize);
    }
    if (rc != NO_ERROR)
        return result;

    for (auto *adapter = addrs; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;

        for (auto *ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            auto *sa = reinterpret_cast<sockaddr_in *>(ua->Address.lpSockaddr);
            if ((ntohl(sa->sin_addr.s_addr) >> 24) == 127)
                continue;
            result.push_back(sa->sin_addr);
        }
    }
#else
    struct ifaddrs *ifa_list = nullptr;
    if (getifaddrs(&ifa_list) != 0)
        return result;

    for (auto *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;

        auto *sa = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
        result.push_back(sa->sin_addr);
    }
    freeifaddrs(ifa_list);
#endif

    return result;
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

void Discovery::send_probes(const std::vector<socket_t> &socks, uint16_t port)
{
    char buf[512];
    snprintf(buf, sizeof(buf), MSEARCH_TEMPLATE, port);
    int msg_len = (int)strlen(buf);

    sockaddr_in mcast_dest{};
    mcast_dest.sin_family = AF_INET;
    mcast_dest.sin_addr.s_addr = inet_addr(SSDP_MULTICAST_ADDR);
    mcast_dest.sin_port = htons(port);

    sockaddr_in bcast_dest{};
    bcast_dest.sin_family = AF_INET;
    bcast_dest.sin_addr.s_addr = inet_addr("255.255.255.255");
    bcast_dest.sin_port = htons(port);

    for (auto sock : socks) {
        // Multicast probe
        int sent = sendto(sock, buf, msg_len, 0, (sockaddr *)&mcast_dest, sizeof(mcast_dest));
        m_diag.last_send_result.store(sent);
        if (sent < 0) {
#ifdef _WIN32
            m_diag.last_send_error.store(WSAGetLastError());
#else
            m_diag.last_send_error.store(errno);
#endif
        }
        m_diag.probes_sent++;

        // Broadcast probe
        sendto(sock, buf, msg_len, 0, (sockaddr *)&bcast_dest, sizeof(bcast_dest));
        m_diag.probes_sent++;
    }
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

    // Create one socket per network interface, each bound to that interface's IP.
    // This ensures the source IP in outgoing probes is correct so printers can
    // send unicast responses back. Critical on multi-homed systems (Hyper-V, WSL, VPN).
    auto ifaces = get_local_ipv4_addresses();
    m_diag.interfaces_found.store((int)ifaces.size());

    std::vector<socket_t> socks;

    for (auto &addr : ifaces) {
        socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCK)
            continue;

        // Bind to this interface's IP on an ephemeral port
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr = addr;
        bind_addr.sin_port = 0;
        if (::bind(sock, (sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            closesocket(sock);
            continue;
        }

        // Enable broadcast
        int bcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&bcast, sizeof(bcast));

        // Non-blocking mode for poll-based receive
#ifdef _WIN32
        u_long nonblock = 1;
        ioctlsocket(sock, FIONBIO, &nonblock);
#else
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

        BOOST_LOG_TRIVIAL(info) << "[SSDP] Created socket on " << inet_ntoa(addr);
        socks.push_back(sock);
    }

    if (socks.empty()) {
        BOOST_LOG_TRIVIAL(error) << "[SSDP] No usable network interfaces found";
        m_running.store(false);
        return;
    }

    // Create multicast listener sockets on ports 2021 and 1990.
    // Bambu printers (especially A1 series) broadcast unsolicited NOTIFY messages
    // to 239.255.255.250 on these ports every ~5 seconds. The A1 Mini does NOT
    // respond to M-SEARCH probes — it only sends NOTIFY. We must listen for these.
    std::vector<socket_t> mcast_socks;
    for (int i = 0; i < NUM_SSDP_PORTS; ++i) {
        socket_t msock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (msock == INVALID_SOCK)
            continue;

        int reuse = 1;
        setsockopt(msock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind_addr.sin_port = htons(SSDP_PORTS[i]);

        if (::bind(msock, (sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            closesocket(msock);
            continue;
        }

        // Join multicast group on every interface
        for (auto &addr : ifaces) {
            ip_mreq mreq{};
            mreq.imr_multiaddr.s_addr = inet_addr(SSDP_MULTICAST_ADDR);
            mreq.imr_interface = addr;
            setsockopt(msock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq));
        }

        // Non-blocking
#ifdef _WIN32
        u_long nonblock = 1;
        ioctlsocket(msock, FIONBIO, &nonblock);
#else
        int flags = fcntl(msock, F_GETFL, 0);
        fcntl(msock, F_SETFL, flags | O_NONBLOCK);
#endif

        BOOST_LOG_TRIVIAL(info) << "[SSDP] Multicast listener on port " << SSDP_PORTS[i];
        mcast_socks.push_back(msock);
    }

    // Combine all sockets for polling: per-interface sockets + multicast listeners
    std::vector<socket_t> all_socks;
    all_socks.insert(all_socks.end(), socks.begin(), socks.end());
    all_socks.insert(all_socks.end(), mcast_socks.begin(), mcast_socks.end());

    // Send initial M-SEARCH probes on all interfaces
    for (int i = 0; i < NUM_SSDP_PORTS; ++i)
        send_probes(socks, SSDP_PORTS[i]);

    auto last_msearch = std::chrono::steady_clock::now();
    m_diag.last_probe_time = last_msearch;
    char buf[4096];

    while (!m_stop_requested.load()) {
        // Poll all sockets for incoming data
        bool got_any = false;

        for (auto sock : all_socks) {
            for (;;) {
                sockaddr_in sender{};
                int sender_len = sizeof(sender);
                int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                                 (sockaddr *)&sender, &sender_len);

                if (n <= 0) {
#ifdef _WIN32
                    int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK)
                        m_diag.recv_errors++;
#endif
                    break; // No more data on this socket
                }

                got_any = true;
                buf[n] = '\0';
                m_diag.packets_received++;
                std::string source_ip = inet_ntoa(sender.sin_addr);
                std::string json = parse_ssdp_message(std::string(buf, n), source_ip);

                if (!json.empty()) {
                    m_diag.bambu_responses++;
                    m_diag.last_response_time = std::chrono::steady_clock::now();

                    // Dedup: only fire callback once per device per interval
                    bool should_fire = true;
                    {
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
                            else
                                m_diag.dedup_suppressed++;
                        }
                    }

                    if (should_fire) {
                        m_diag.callbacks_fired++;
                        std::lock_guard<std::mutex> lock(m_callback_mutex);
                        if (m_callback)
                            m_callback(json);
                    }
                }
            }
        }

        if (!got_any) {
            m_diag.recv_timeouts++;
            // Sleep briefly to avoid busy-spinning when no data
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50000);
#endif
        }

        // Re-send M-SEARCH every 20 seconds
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_msearch).count() >= 20) {
            for (int i = 0; i < NUM_SSDP_PORTS; ++i)
                send_probes(socks, SSDP_PORTS[i]);
            m_diag.last_probe_time = now;
            last_msearch = now;
        }
    }

    for (auto sock : all_socks)
        closesocket(sock);

    m_running.store(false);
}

} // namespace OpenBambu
