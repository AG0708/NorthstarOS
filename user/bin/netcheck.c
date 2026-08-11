#include <northstar_user.h>

#include <stdint.h>

#define NETWORK_TIMEOUT_NS UINT64_C(10000000000)

static int fail(const char *reason)
{
    dprintf(STDERR_FILENO, "NORTHSTAR:NETCHECK:FAIL %s errno=%d\n", reason,
            errno);
    return 1;
}

int main(void)
{
    static const char udp_payload[] = "northstar-ring3-udp";
    static const char request[] =
        "GET /evidence HTTP/1.1\r\n"
        "Host: northstar.test\r\n"
        "Connection: close\r\n\r\n";
    static const char expected[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 21\r\n"
        "Connection: close\r\n\r\n"
        "northstar-network-ok\n";
    const struct ns_abi_socket_address udp_peer = {
        .address = NS_ABI_IPV4_ADDRESS(10, 0, 2, 2),
        .port = 9000,
    };
    const struct ns_abi_socket_address tcp_peer = {
        .address = NS_ABI_IPV4_ADDRESS(10, 0, 2, 2),
        .port = 8080,
    };
    const struct ns_abi_socket_address *invalid_address =
        (const struct ns_abi_socket_address *)(uintptr_t)
            UINT64_C(0xffff800000000000);
    const void *invalid_buffer =
        (const void *)(uintptr_t)UINT64_C(0xffff800000000000);
    struct ns_abi_socket_address source;
    char udp_response[sizeof(udp_payload)];
    char response[256];
    size_t received = 0;
    int descriptor;
    int64_t count;

    descriptor = socket_open(NS_ABI_AF_INET, NS_ABI_SOCK_DGRAM,
                             NS_ABI_IPPROTO_UDP);
    if (descriptor < 0 ||
        socket_set_timeouts(descriptor, NETWORK_TIMEOUT_NS,
                            NETWORK_TIMEOUT_NS) != 0)
        return fail("udp-open");

    errno = 0;
    if (socket_connect(descriptor, invalid_address) != -1 ||
        errno != NS_EFAULT)
        return fail("connect-copyin");
    errno = 0;
    if (socket_sendto(descriptor, invalid_buffer, 1, &udp_peer) != -1 ||
        errno != NS_EFAULT)
        return fail("send-copyin");
    puts("# NS_TEST net.ring3-usercopy PASS");

    count = socket_sendto(descriptor, udp_payload, sizeof(udp_payload) - 1u,
                          &udp_peer);
    if (count != (int64_t)(sizeof(udp_payload) - 1u))
        return fail("udp-send");
    count = socket_recvfrom(descriptor, udp_response, sizeof(udp_response),
                            &source);
    if (count != (int64_t)(sizeof(udp_payload) - 1u) ||
        memcmp(udp_response, udp_payload, sizeof(udp_payload) - 1u) != 0 ||
        source.address != udp_peer.address || source.port != udp_peer.port ||
        source.reserved != 0 || socket_close(descriptor) != 0)
        return fail("udp-receive");
    puts("# NS_TEST net.udp PASS");

    descriptor = socket_open(NS_ABI_AF_INET, NS_ABI_SOCK_STREAM,
                             NS_ABI_IPPROTO_TCP);
    if (descriptor < 0 ||
        socket_set_timeouts(descriptor, NETWORK_TIMEOUT_NS,
                            NETWORK_TIMEOUT_NS) != 0 ||
        socket_connect(descriptor, &tcp_peer) != 0)
        return fail("tcp-connect");
    count = socket_send(descriptor, request, sizeof(request) - 1u);
    if (count != (int64_t)(sizeof(request) - 1u))
        return fail("http-send");
    while (received < sizeof(expected) - 1u) {
        count = socket_recv(descriptor, response + received,
                            sizeof(response) - received);
        if (count <= 0)
            return fail("http-receive");
        received += (size_t)count;
        if (received > sizeof(expected) - 1u)
            return fail("http-trailing-bytes");
    }
    if (received != sizeof(expected) - 1u ||
        memcmp(response, expected, sizeof(expected) - 1u) != 0 ||
        socket_close(descriptor) != 0)
        return fail("http-response");

    puts("# NS_TEST net.tcp PASS");
    puts("NS:NET:HTTP:RESPONSE status=200 "
         "body_sha256=5ebbf3b82d2e188ee3abf4547bf3a4a8dbf557a7f939cb5d817542f7385df943");
    puts("# NS_TEST net.http PASS");
    puts("NS:NET:RING3 program=/bin/netcheck syscall_copyin=contained");
    puts("# NS_TEST net.ring3-program PASS");
    return 0;
}
