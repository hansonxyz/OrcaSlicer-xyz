#include "OpenBambuMqtt.hpp"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  define closesocket close
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#include <cstring>
#include <sstream>
#include <chrono>

namespace OpenBambu {

// MQTT packet types (high nibble of first byte)
static constexpr uint8_t MQTT_CONNECT     = 0x10;
static constexpr uint8_t MQTT_CONNACK     = 0x20;
static constexpr uint8_t MQTT_PUBLISH     = 0x30;
static constexpr uint8_t MQTT_SUBSCRIBE   = 0x82; // with QoS 1 flag
static constexpr uint8_t MQTT_SUBACK      = 0x90;
static constexpr uint8_t MQTT_PINGREQ     = 0xC0;
static constexpr uint8_t MQTT_PINGRESP    = 0xD0;
static constexpr uint8_t MQTT_DISCONNECT  = 0xE0;

MqttClient::MqttClient()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

MqttClient::~MqttClient()
{
    disconnect();
}

void MqttClient::set_on_message(OnMessage cb)
{
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_on_message = std::move(cb);
}

void MqttClient::set_on_connect(OnConnect cb)
{
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_on_connect = std::move(cb);
}

std::vector<uint8_t> MqttClient::encode_remaining_length(uint32_t len)
{
    std::vector<uint8_t> result;
    do {
        uint8_t byte = len & 0x7F;
        len >>= 7;
        if (len > 0) byte |= 0x80;
        result.push_back(byte);
    } while (len > 0);
    return result;
}

std::vector<uint8_t> MqttClient::build_connect_packet(
    const std::string &client_id, const std::string &username, const std::string &password)
{
    // Variable header: protocol name "MQTT", protocol level 4, connect flags, keepalive
    std::vector<uint8_t> var_header;
    // Protocol name: length(2) + "MQTT"
    var_header.push_back(0x00); var_header.push_back(0x04);
    var_header.push_back('M'); var_header.push_back('Q');
    var_header.push_back('T'); var_header.push_back('T');
    // Protocol level: 4 (MQTT 3.1.1)
    var_header.push_back(0x04);
    // Connect flags: username + password + clean session
    var_header.push_back(0xC2); // 11000010: username, password, clean session
    // Keep alive: 30 seconds
    var_header.push_back(0x00); var_header.push_back(30);

    // Payload: client_id, username, password (each prefixed with 2-byte length)
    std::vector<uint8_t> payload;
    auto add_string = [&payload](const std::string &s) {
        payload.push_back((uint8_t)(s.size() >> 8));
        payload.push_back((uint8_t)(s.size() & 0xFF));
        payload.insert(payload.end(), s.begin(), s.end());
    };
    add_string(client_id);
    add_string(username);
    add_string(password);

    uint32_t remaining = (uint32_t)(var_header.size() + payload.size());
    auto rem_enc = encode_remaining_length(remaining);

    std::vector<uint8_t> packet;
    packet.push_back(MQTT_CONNECT);
    packet.insert(packet.end(), rem_enc.begin(), rem_enc.end());
    packet.insert(packet.end(), var_header.begin(), var_header.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

std::vector<uint8_t> MqttClient::build_subscribe_packet(uint16_t packet_id, const std::string &topic)
{
    std::vector<uint8_t> var_payload;
    // Packet identifier
    var_payload.push_back((uint8_t)(packet_id >> 8));
    var_payload.push_back((uint8_t)(packet_id & 0xFF));
    // Topic filter: length(2) + topic + QoS(1)
    var_payload.push_back((uint8_t)(topic.size() >> 8));
    var_payload.push_back((uint8_t)(topic.size() & 0xFF));
    var_payload.insert(var_payload.end(), topic.begin(), topic.end());
    var_payload.push_back(0x00); // QoS 0

    auto rem_enc = encode_remaining_length((uint32_t)var_payload.size());

    std::vector<uint8_t> packet;
    packet.push_back(MQTT_SUBSCRIBE);
    packet.insert(packet.end(), rem_enc.begin(), rem_enc.end());
    packet.insert(packet.end(), var_payload.begin(), var_payload.end());
    return packet;
}

std::vector<uint8_t> MqttClient::build_publish_packet(const std::string &topic, const std::string &payload)
{
    std::vector<uint8_t> var_payload;
    // Topic: length(2) + topic
    var_payload.push_back((uint8_t)(topic.size() >> 8));
    var_payload.push_back((uint8_t)(topic.size() & 0xFF));
    var_payload.insert(var_payload.end(), topic.begin(), topic.end());
    // No packet identifier for QoS 0
    // Payload
    var_payload.insert(var_payload.end(), payload.begin(), payload.end());

    auto rem_enc = encode_remaining_length((uint32_t)var_payload.size());

    std::vector<uint8_t> packet;
    packet.push_back(MQTT_PUBLISH); // QoS 0, no retain, no dup
    packet.insert(packet.end(), rem_enc.begin(), rem_enc.end());
    packet.insert(packet.end(), var_payload.begin(), var_payload.end());
    return packet;
}

std::vector<uint8_t> MqttClient::build_pingreq_packet()
{
    return {MQTT_PINGREQ, 0x00};
}

std::vector<uint8_t> MqttClient::build_disconnect_packet()
{
    return {MQTT_DISCONNECT, 0x00};
}

bool MqttClient::tls_read(void *buf, int n)
{
    int total = 0;
    while (total < n) {
        int r = SSL_read((SSL *)m_ssl, (char *)buf + total, n - total);
        if (r <= 0) return false;
        total += r;
    }
    return true;
}

bool MqttClient::tls_write(const void *buf, int n)
{
    std::lock_guard<std::mutex> lock(m_write_mutex);
    int total = 0;
    while (total < n) {
        int w = SSL_write((SSL *)m_ssl, (const char *)buf + total, n - total);
        if (w <= 0) return false;
        total += w;
    }
    return true;
}

void MqttClient::close_socket()
{
    if (m_ssl) {
        SSL_shutdown((SSL *)m_ssl);
        SSL_free((SSL *)m_ssl);
        m_ssl = nullptr;
    }
    if (m_ssl_ctx) {
        SSL_CTX_free((SSL_CTX *)m_ssl_ctx);
        m_ssl_ctx = nullptr;
    }
    if (m_socket >= 0) {
        closesocket(m_socket);
        m_socket = -1;
    }
}

bool MqttClient::connect(const std::string &host, uint16_t port,
                          const std::string &serial,
                          const std::string &access_code,
                          const std::string &ca_pem_path)
{
    if (m_connected.load()) disconnect();

    // Save connection params for auto-reconnect
    m_host = host;
    m_port = port;
    m_access_code = access_code;
    m_ca_pem_path = ca_pem_path;
    m_reconnect_attempts = 0;
    m_auto_reconnect = true;

    m_serial = serial;
    m_report_topic = "device/" + serial + "/report";
    m_request_topic = "device/" + serial + "/request";
    m_stop_requested.store(false);

    // Create TCP socket
#ifdef _WIN32
    m_socket = (int)::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    m_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    if (m_socket < 0) {
        fprintf(stderr, "MQTT: socket() failed\n");
        return false;
    }

    // Resolve hostname
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *he = gethostbyname(host.c_str());
        if (!he) {
            fprintf(stderr, "MQTT: DNS resolution failed for %s\n", host.c_str());
            closesocket(m_socket); m_socket = -1;
            return false;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    // Set socket timeout for connect
#ifdef _WIN32
    DWORD timeout_ms = 5000;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    // TCP connect
    if (::connect(m_socket, (sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "MQTT: TCP connect to %s:%d failed\n", host.c_str(), port);
        closesocket(m_socket); m_socket = -1;
        return false;
    }

    // Set up TLS
    const SSL_METHOD *method = TLS_client_method();
    m_ssl_ctx = SSL_CTX_new(method);
    if (!m_ssl_ctx) {
        fprintf(stderr, "MQTT: SSL_CTX_new failed\n");
        closesocket(m_socket); m_socket = -1;
        return false;
    }

    // Load CA certificate for verification (optional — skip verify if no CA provided)
    if (!ca_pem_path.empty()) {
        if (SSL_CTX_load_verify_locations((SSL_CTX *)m_ssl_ctx, ca_pem_path.c_str(), nullptr) != 1) {
            fprintf(stderr, "MQTT: WARNING: Failed to load CA cert from %s, disabling verification\n", ca_pem_path.c_str());
            SSL_CTX_set_verify((SSL_CTX *)m_ssl_ctx, SSL_VERIFY_NONE, nullptr);
        } else {
            SSL_CTX_set_verify((SSL_CTX *)m_ssl_ctx, SSL_VERIFY_PEER, nullptr);
        }
    } else {
        SSL_CTX_set_verify((SSL_CTX *)m_ssl_ctx, SSL_VERIFY_NONE, nullptr);
    }

    m_ssl = SSL_new((SSL_CTX *)m_ssl_ctx);
    if (!m_ssl) {
        fprintf(stderr, "MQTT: SSL_new failed\n");
        SSL_CTX_free((SSL_CTX *)m_ssl_ctx); m_ssl_ctx = nullptr;
        closesocket(m_socket); m_socket = -1;
        return false;
    }

    // Set SNI to the printer's serial number (required by Bambu firmware)
    SSL_set_tlsext_host_name((SSL *)m_ssl, serial.c_str());
    SSL_set_fd((SSL *)m_ssl, m_socket);

    // TLS handshake
    if (SSL_connect((SSL *)m_ssl) != 1) {
        fprintf(stderr, "MQTT: TLS handshake failed\n");
        unsigned long err;
        while ((err = ERR_get_error()) != 0) {
            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            fprintf(stderr, "  OpenSSL: %s\n", buf);
        }
        SSL_free((SSL *)m_ssl); m_ssl = nullptr;
        SSL_CTX_free((SSL_CTX *)m_ssl_ctx); m_ssl_ctx = nullptr;
        closesocket(m_socket); m_socket = -1;
        return false;
    }

    // Send MQTT CONNECT
    std::string client_id = "openbambu-" + serial.substr(serial.size() > 6 ? serial.size() - 6 : 0);
    auto connect_pkt = build_connect_packet(client_id, "bblp", access_code);
    if (!tls_write(connect_pkt.data(), (int)connect_pkt.size())) {
        fprintf(stderr, "MQTT: Failed to send CONNECT\n");
        disconnect();
        return false;
    }

    // Read CONNACK (4 bytes: type + remaining_length + flags + return_code)
    uint8_t connack[4];
    if (!tls_read(connack, 4)) {
        fprintf(stderr, "MQTT: Failed to read CONNACK\n");
        disconnect();
        return false;
    }
    if ((connack[0] & 0xF0) != MQTT_CONNACK || connack[3] != 0) {
        fprintf(stderr, "MQTT: CONNACK failed, return code=%d\n", connack[3]);
        disconnect();
        return false;
    }

    fprintf(stderr, "MQTT: Connected to %s:%d as %s\n", host.c_str(), port, client_id.c_str());

    // Subscribe to the report topic
    auto sub_pkt = build_subscribe_packet(m_next_packet_id++, m_report_topic);
    if (!tls_write(sub_pkt.data(), (int)sub_pkt.size())) {
        fprintf(stderr, "MQTT: Failed to send SUBSCRIBE\n");
        disconnect();
        return false;
    }

    m_connected.store(true);

    // Fire connect callback
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_on_connect) m_on_connect(true);
    }

    // Spawn reader thread
    m_reader_thread = std::thread(&MqttClient::reader_thread_func, this);

    return true;
}

void MqttClient::disconnect()
{
    m_auto_reconnect = false; // explicit disconnect — don't reconnect
    m_stop_requested.store(true);

    if (m_ssl && m_connected.load()) {
        auto disc_pkt = build_disconnect_packet();
        tls_write(disc_pkt.data(), (int)disc_pkt.size());
    }

    bool was_connected = m_connected.exchange(false);

    if (m_reader_thread.joinable())
        m_reader_thread.join();

    close_socket();

    // Fire disconnect callback only if we were actually connected
    if (was_connected) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_on_connect) m_on_connect(false);
    }
}

bool MqttClient::reconnect()
{
    if (m_stop_requested.load() || m_host.empty())
        return false;

    if (m_reconnect_attempts >= MAX_RECONNECT_ATTEMPTS) {
        fprintf(stderr, "MQTT: max reconnect attempts (%d) reached, giving up\n", MAX_RECONNECT_ATTEMPTS);
        return false;
    }

    ++m_reconnect_attempts;
    fprintf(stderr, "MQTT: reconnecting (attempt %d/%d) in %ds...\n",
            m_reconnect_attempts, MAX_RECONNECT_ATTEMPTS, RECONNECT_DELAY_SEC);

    // Wait before reconnecting (check stop_requested periodically)
    for (int i = 0; i < RECONNECT_DELAY_SEC && !m_stop_requested.load(); ++i)
        std::this_thread::sleep_for(std::chrono::seconds(1));

    if (m_stop_requested.load()) return false;

    // Close existing socket/SSL if any
    close_socket();

    // Re-do the full connection sequence (TCP + TLS + MQTT CONNECT + SUBSCRIBE)
    // This duplicates connect() logic but without spawning a new reader thread
    // (we're already in the reader thread).

#ifdef _WIN32
    m_socket = (int)::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    m_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    if (m_socket < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    addr.sin_addr.s_addr = inet_addr(m_host.c_str());

#ifdef _WIN32
    DWORD timeout_ms = 5000;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (::connect(m_socket, (sockaddr *)&addr, sizeof(addr)) < 0) {
        close_socket();
        return false;
    }

    m_ssl_ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify((SSL_CTX *)m_ssl_ctx, SSL_VERIFY_NONE, nullptr);
    m_ssl = SSL_new((SSL_CTX *)m_ssl_ctx);
    SSL_set_tlsext_host_name((SSL *)m_ssl, m_serial.c_str());
    SSL_set_fd((SSL *)m_ssl, m_socket);

    if (SSL_connect((SSL *)m_ssl) != 1) {
        close_socket();
        return false;
    }

    std::string client_id = "openbambu-" + m_serial.substr(m_serial.size() > 6 ? m_serial.size() - 6 : 0);
    auto connect_pkt = build_connect_packet(client_id, "bblp", m_access_code);
    if (!tls_write(connect_pkt.data(), (int)connect_pkt.size())) {
        close_socket();
        return false;
    }

    uint8_t connack[4];
    if (!tls_read(connack, 4) || (connack[0] & 0xF0) != MQTT_CONNACK || connack[3] != 0) {
        close_socket();
        return false;
    }

    auto sub_pkt = build_subscribe_packet(m_next_packet_id++, m_report_topic);
    if (!tls_write(sub_pkt.data(), (int)sub_pkt.size())) {
        close_socket();
        return false;
    }

    m_connected.store(true);
    m_reconnect_attempts = 0; // reset on success
    fprintf(stderr, "MQTT: reconnected successfully\n");

    // Fire connect callback
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_on_connect) m_on_connect(true);
    }

    // Request fresh status
    request_pushall();

    return true;
}

bool MqttClient::publish(const std::string &payload)
{
    if (!m_connected.load()) return false;
    auto pkt = build_publish_packet(m_request_topic, payload);
    return tls_write(pkt.data(), (int)pkt.size());
}

bool MqttClient::request_pushall()
{
    std::ostringstream json;
    json << "{\"pushing\":{\"sequence_id\":\"" << m_sequence_id++
         << "\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";
    return publish(json.str());
}

void MqttClient::handle_packet(uint8_t type, const std::vector<uint8_t> &data)
{
    uint8_t ptype = type & 0xF0;

    if (ptype == MQTT_PUBLISH) {
        // Parse PUBLISH: topic_len(2) + topic + [packet_id if QoS>0] + payload
        if (data.size() < 2) return;
        uint16_t topic_len = ((uint16_t)data[0] << 8) | data[1];
        if (data.size() < 2u + topic_len) return;
        std::string topic((const char *)data.data() + 2, topic_len);

        size_t payload_offset = 2 + topic_len;
        // QoS from fixed header bits 1-2
        uint8_t qos = (type >> 1) & 0x03;
        if (qos > 0) payload_offset += 2; // skip packet identifier

        std::string payload;
        if (payload_offset < data.size())
            payload.assign((const char *)data.data() + payload_offset, data.size() - payload_offset);

        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_on_message)
            m_on_message(topic, payload);
    }
    else if (ptype == MQTT_SUBACK) {
        // SUBACK received — subscription confirmed
        if (data.size() >= 3) {
            uint8_t rc = data[2]; // return code: 0=QoS0, 1=QoS1, 2=QoS2, 0x80=failure
            if (rc == 0x80)
                fprintf(stderr, "MQTT: Subscription rejected\n");
        }
    }
    else if (ptype == MQTT_PINGRESP) {
        // Ping response — connection alive
    }
    else if (ptype == MQTT_CONNACK) {
        // Already handled in connect()
    }
}

void MqttClient::reader_thread_func()
{
    while (!m_stop_requested.load()) {
        auto last_ping = std::chrono::steady_clock::now();

        // Inner loop: read packets while connected
        while (!m_stop_requested.load() && m_connected.load()) {
            uint8_t first_byte;
            if (!tls_read(&first_byte, 1)) {
                // Read timeout or disconnect — check if socket is still alive
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count();

                if (elapsed >= 15) {
                    auto ping = build_pingreq_packet();
                    if (!tls_write(ping.data(), (int)ping.size())) {
                        fprintf(stderr, "MQTT: ping failed, connection lost\n");
                        m_connected.store(false);
                        break;
                    }
                    last_ping = now;
                }
                continue;
            }

            // Read remaining length
            uint32_t remaining = 0;
            uint32_t multiplier = 1;
            bool read_ok = true;
            for (int i = 0; i < 4; ++i) {
                uint8_t byte;
                if (!tls_read(&byte, 1)) { read_ok = false; break; }
                remaining += (byte & 0x7F) * multiplier;
                multiplier *= 128;
                if ((byte & 0x80) == 0) break;
            }
            if (!read_ok) { m_connected.store(false); break; }

            // Read payload
            std::vector<uint8_t> data(remaining);
            if (remaining > 0 && !tls_read(data.data(), (int)remaining)) {
                m_connected.store(false);
                break;
            }

            handle_packet(first_byte, data);
            last_ping = std::chrono::steady_clock::now();
            m_reconnect_attempts = 0; // successful data resets reconnect counter
        }

        // Connection lost — try to reconnect if auto_reconnect is enabled
        if (m_stop_requested.load() || !m_auto_reconnect)
            break;

        // Fire disconnect callback
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            if (m_on_connect) m_on_connect(false);
        }

        if (!reconnect())
            break; // reconnect failed or gave up
    }
}

} // namespace OpenBambu
