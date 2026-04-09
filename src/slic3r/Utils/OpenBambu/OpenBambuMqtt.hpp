#pragma once

// OpenBambu MQTT Client
//
// Minimal MQTT 3.1.1 client for Bambu Lab printer communication over LAN.
// Uses OpenSSL for TLS. Implements only the subset of MQTT needed:
//   CONNECT, CONNACK, SUBSCRIBE, SUBACK, PUBLISH (receive), PINGREQ/PINGRESP, DISCONNECT
//
// Connection: TLS on port 8883, username "bblp", password = LAN access code.
// Subscribe: device/{serial}/report
// Publish:   device/{serial}/request

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

namespace OpenBambu {

class MqttClient {
public:
    using OnMessage = std::function<void(const std::string &topic, const std::string &payload)>;
    using OnConnect = std::function<void(bool connected)>;

    MqttClient();
    ~MqttClient();

    // Set callbacks
    void set_on_message(OnMessage cb);
    void set_on_connect(OnConnect cb);

    // Connect to a printer. Blocks briefly for TLS handshake, then spawns
    // a background thread for message reception.
    // ca_pem_path: path to BBL CA certificate bundle (PEM format)
    // Returns true if connection initiated successfully.
    bool connect(const std::string &host, uint16_t port,
                 const std::string &serial,
                 const std::string &access_code,
                 const std::string &ca_pem_path = "");

    // Disconnect and stop the background thread.
    void disconnect();

    // Publish a message to the printer's request topic.
    bool publish(const std::string &payload);

    // Send the pushall command to request a full status dump.
    bool request_pushall();

    bool is_connected() const { return m_connected.load(); }

private:
    // MQTT wire protocol helpers
    std::vector<uint8_t> build_connect_packet(const std::string &client_id,
                                               const std::string &username,
                                               const std::string &password);
    std::vector<uint8_t> build_subscribe_packet(uint16_t packet_id, const std::string &topic);
    std::vector<uint8_t> build_publish_packet(const std::string &topic, const std::string &payload);
    std::vector<uint8_t> build_pingreq_packet();
    std::vector<uint8_t> build_disconnect_packet();

    // Encode MQTT remaining length (variable-length encoding)
    static std::vector<uint8_t> encode_remaining_length(uint32_t len);

    // Background thread: reads incoming MQTT packets
    void reader_thread_func();

    // Read exactly n bytes from TLS, returns false on error/disconnect
    bool tls_read(void *buf, int n);
    bool tls_write(const void *buf, int n);

    // Parse a received MQTT packet
    void handle_packet(uint8_t type, const std::vector<uint8_t> &data);

    OnMessage m_on_message;
    OnConnect m_on_connect;
    std::mutex m_callback_mutex;

    std::string m_serial;
    std::string m_report_topic;  // device/{serial}/report
    std::string m_request_topic; // device/{serial}/request

    // Auto-reconnect: attempt to reconnect on connection loss
    bool reconnect();
    void close_socket(); // close SSL/socket without firing callbacks

    // Connection parameters (saved for reconnect)
    std::string m_host;
    uint16_t m_port = 0;
    std::string m_access_code;
    std::string m_ca_pem_path;

    // OpenSSL handles (opaque pointers to avoid including openssl headers)
    void *m_ssl_ctx = nullptr; // SSL_CTX*
    void *m_ssl = nullptr;     // SSL*
    int m_socket = -1;

    std::thread m_reader_thread;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_stop_requested{false};
    std::mutex m_write_mutex; // serialize TLS writes

    uint16_t m_next_packet_id = 1;
    int m_sequence_id = 0;
    bool m_auto_reconnect = true;
    int m_reconnect_attempts = 0;
    static constexpr int MAX_RECONNECT_ATTEMPTS = 10;
    static constexpr int RECONNECT_DELAY_SEC = 3;
};

} // namespace OpenBambu
