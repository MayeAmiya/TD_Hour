#pragma once

#include "container/container_types.h"

#include <enet/enet.h>
#include <cstdint>
namespace platform {

// ── ENet 全局生命周期 ──────────────────────────────────────────────────
class net_init
{
public:
    net_init() noexcept { enet_initialize(); }
    ~net_init() noexcept { enet_deinitialize(); }
    net_init(const net_init&) = delete;
    net_init& operator=(const net_init&) = delete;
};

// ── 地址 ────────────────────────────────────────────────────────────────
struct address
{
    ENetAddress addr{};

    address() noexcept = default;

    address(const char* hostname, uint16_t port) noexcept
    {
        enet_address_set_host(&addr, hostname);
        addr.port = port;
    }

    address(uint32_t ip, uint16_t port) noexcept
    {
        addr.host = ip;
        addr.port = port;
    }

    [[nodiscard]] uint32_t ip() const noexcept { return addr.host; }
    [[nodiscard]] uint16_t port() const noexcept { return addr.port; }

    [[nodiscard]] bool set_host(const char* hostname) noexcept
    {
        return enet_address_set_host(&addr, hostname) == 0;
    }

    [[nodiscard]] container::String to_string() const
    {
        char buf[64];
        enet_address_get_host_ip(&addr, buf, sizeof(buf));
        return buf;
    }
};

// ── 数据包 ──────────────────────────────────────────────────────────────
class packet
{
public:
    packet() noexcept = default;

    explicit packet(container::Span<const uint8_t> data, int flags = ENET_PACKET_FLAG_RELIABLE) noexcept
        : pkt_(enet_packet_create(data.data(), data.size(), flags))
    {
    }

    explicit packet(const void* data, size_t size, int flags = ENET_PACKET_FLAG_RELIABLE) noexcept
        : pkt_(enet_packet_create(data, size, flags))
    {
    }

    ~packet() noexcept { destroy(); }

    packet(packet&& other) noexcept : pkt_(other.pkt_) { other.pkt_ = nullptr; }
    packet& operator=(packet&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            pkt_ = other.pkt_;
            other.pkt_ = nullptr;
        }
        return *this;
    }

    packet(const packet&) = delete;
    packet& operator=(const packet&) = delete;

    void destroy() noexcept
    {
        if (pkt_) { enet_packet_destroy(pkt_); pkt_ = nullptr; }
    }

    [[nodiscard]] ENetPacket* release() noexcept
    {
        ENetPacket* p = pkt_;
        pkt_ = nullptr;
        return p;
    }

    [[nodiscard]] ENetPacket* raw() noexcept { return pkt_; }
    [[nodiscard]] const ENetPacket* raw() const noexcept { return pkt_; }

private:
    ENetPacket* pkt_ = nullptr;
};

// ── 对端 ────────────────────────────────────────────────────────────────
class peer
{
public:
    peer() noexcept = default;
    explicit peer(ENetPeer* p) noexcept : peer_(p) {}

    // 通过 host::connect() 创建连接，peer 自身不能发起连接
    void disconnect(uint32_t data = 0) noexcept
    {
        if (peer_) { enet_peer_disconnect(peer_, data); }
    }

    void disconnect_now(uint32_t data = 0) noexcept
    {
        if (peer_) { enet_peer_disconnect_now(peer_, data); }
    }

    void send(uint8_t channel, packet&& pkt, int flags = ENET_PACKET_FLAG_RELIABLE) noexcept
    {
        if (peer_)
        {
            enet_peer_send(peer_, channel, pkt.release());
        }
    }

    [[nodiscard]] ENetPeer* raw() noexcept { return peer_; }
    [[nodiscard]] const ENetPeer* raw() const noexcept { return peer_; }
    [[nodiscard]] explicit operator bool() const noexcept { return peer_ != nullptr; }

private:
    ENetPeer* peer_ = nullptr;
};

// ── 主机（服务端/客户端） ─────────────────────────────────────────────
class host
{
public:
    host() noexcept = default;

    bool create_client(const address* bind_addr = nullptr,
                       size_t peer_count = 1,
                       size_t channel_count = 1,
                       uint32_t incoming_bandwidth = 0,
                       uint32_t outgoing_bandwidth = 0) noexcept
    {
        host_ = enet_host_create(
            bind_addr ? &bind_addr->addr : nullptr,
            static_cast<int>(peer_count),
            static_cast<int>(channel_count),
            incoming_bandwidth, outgoing_bandwidth);
        return host_ != nullptr;
    }

    bool create_server(const address& bind_addr,
                       size_t peer_count = 32,
                       size_t channel_count = 1,
                       uint32_t incoming_bandwidth = 0,
                       uint32_t outgoing_bandwidth = 0) noexcept
    {
        host_ = enet_host_create(
            &bind_addr.addr,
            static_cast<int>(peer_count),
            static_cast<int>(channel_count),
            incoming_bandwidth, outgoing_bandwidth);
        return host_ != nullptr;
    }

    ~host() noexcept { destroy(); }

    host(host&& other) noexcept : host_(other.host_) { other.host_ = nullptr; }
    host& operator=(host&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            host_ = other.host_;
            other.host_ = nullptr;
        }
        return *this;
    }

    host(const host&) = delete;
    host& operator=(const host&) = delete;

    void destroy() noexcept
    {
        if (host_) { enet_host_destroy(host_); host_ = nullptr; }
    }

    // 轮询事件，返回 true 表示有事件
    bool service(ENetEvent& evt, uint32_t timeout_ms = 0) noexcept
    {
        return enet_host_service(host_, &evt, timeout_ms) > 0;
    }

    peer connect(const address& addr, size_t channel_count = 1, uint32_t data = 0) noexcept
    {
        ENetPeer* p = enet_host_connect(host_, &addr.addr,
                                        static_cast<int>(channel_count), data);
        return peer(p);
    }

    void broadcast(uint8_t channel, packet&& pkt) noexcept
    {
        enet_host_broadcast(host_, channel, pkt.release());
    }

    void flush() noexcept { enet_host_flush(host_); }
    void compress_with_range_coder() noexcept { enet_host_compress_with_range_coder(host_); }

    [[nodiscard]] ENetHost* raw() noexcept { return host_; }
    [[nodiscard]] const ENetHost* raw() const noexcept { return host_; }
    [[nodiscard]] explicit operator bool() const noexcept { return host_ != nullptr; }

private:
    ENetHost* host_ = nullptr;
};

} // namespace platform
