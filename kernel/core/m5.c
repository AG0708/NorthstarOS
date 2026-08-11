#include <northstar/arch_cpu.h>
#include <northstar/arch_interrupt.h>
#include <northstar/arch_io.h>
#include <northstar/arch_pic.h>
#include <northstar/arch_timer.h>
#include <northstar/kernel.h>
#include <northstar/mm_pmm.h>
#include <northstar/mm_runtime.h>
#include <northstar/mm_vmm.h>
#include <northstar/net_dns.h>
#include <northstar/net_icmp.h>
#include <northstar/net_stack.h>
#include <northstar/net_tcp.h>
#include <northstar/net_types.h>
#include <northstar/socket_api.h>

#include <stddef.h>

enum {
    M5_PIT_HZ = 1000,
    M5_NETWORK_TIMEOUT_MS = 10000,
};

#define M5_DMA_LIMIT UINT64_C(0x100000000)
#define M5_NANOSECONDS_PER_MILLISECOND UINT64_C(1000000)

static struct net_stack m5_stack;
static struct ns_socket_table m5_sockets;
static volatile uint32_t m5_pci_lock_word;
static uint64_t m5_pci_irq_flags;
static volatile uint32_t m5_dns_complete;
static volatile uint32_t m5_icmp_complete;
static struct net_dns_result m5_dns_result;
static uint16_t m5_icmp_identifier;
static uint16_t m5_icmp_sequence;
static uint8_t m5_icmp_payload[32];
static size_t m5_icmp_payload_length;

extern int northstar_m3_run_netcheck(struct ns_socket_table *sockets);

static NS_NORETURN void m5_fail(const char *reason)
{
    arch_irq_disable();
    klog("m5", reason);
    serial_write("# NS_GATE G7 FAIL\n");
    serial_write("NS:RUN:COMPLETE\n");
    kernel_debug_exit(0x17);
}

static void write_decimal(uint64_t value)
{
    char digits[20];
    size_t count = 0;

    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value != 0) {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    while (count != 0)
        serial_putc(digits[--count]);
}

static uint8_t m5_in8(void *context, uint16_t port)
{
    (void)context;
    return arch_in8(port);
}

static uint16_t m5_in16(void *context, uint16_t port)
{
    (void)context;
    return arch_in16(port);
}

static uint32_t m5_in32(void *context, uint16_t port)
{
    (void)context;
    return arch_in32(port);
}

static void m5_out8(void *context, uint16_t port, uint8_t value)
{
    (void)context;
    arch_out8(port, value);
}

static void m5_out16(void *context, uint16_t port, uint16_t value)
{
    (void)context;
    arch_out16(port, value);
}

static void m5_out32(void *context, uint16_t port, uint32_t value)
{
    (void)context;
    arch_out32(port, value);
}

static void *m5_dma_alloc(void *context, size_t bytes, size_t alignment,
                          uint64_t *physical_address)
{
    struct ns_pmm *pmm = ns_mm_runtime_pmm();
    struct ns_vmm *vmm = ns_mm_runtime_vmm();
    uint64_t physical;
    uint64_t span;
    size_t pages;
    size_t alignment_pages;
    void *result;

    (void)context;
    if (pmm == NULL || vmm == NULL || physical_address == NULL || bytes == 0 ||
        alignment == 0 || (alignment & (alignment - 1u)) != 0 ||
        bytes > SIZE_MAX - (size_t)NS_PAGE_SIZE + 1u)
        return NULL;
    pages = (bytes + (size_t)NS_PAGE_SIZE - 1u) / (size_t)NS_PAGE_SIZE;
    alignment_pages =
        (alignment + (size_t)NS_PAGE_SIZE - 1u) / (size_t)NS_PAGE_SIZE;
    if (alignment_pages == 0)
        alignment_pages = 1;
    if (ns_pmm_alloc_pages(pmm, pages, alignment_pages, M5_DMA_LIMIT,
                           &physical) != NS_PMM_OK)
        return NULL;
    span = (uint64_t)pages * NS_PAGE_SIZE;
    if (physical >= vmm->config.direct_map_size ||
        span > vmm->config.direct_map_size - physical) {
        (void)ns_pmm_free_pages(pmm, physical, pages);
        return NULL;
    }
    result = (void *)(uintptr_t)(vmm->config.direct_map_base + physical);
    memset(result, 0, (size_t)span);
    *physical_address = physical;
    return result;
}

static void m5_dma_free(void *context, void *virtual_address, size_t bytes)
{
    struct ns_pmm *pmm = ns_mm_runtime_pmm();
    struct ns_vmm *vmm = ns_mm_runtime_vmm();
    uintptr_t address = (uintptr_t)virtual_address;
    uint64_t physical;
    size_t pages;

    (void)context;
    if (pmm == NULL || vmm == NULL || virtual_address == NULL || bytes == 0 ||
        address < vmm->config.direct_map_base ||
        bytes > SIZE_MAX - (size_t)NS_PAGE_SIZE + 1u)
        return;
    physical = (uint64_t)(address - vmm->config.direct_map_base);
    pages = (bytes + (size_t)NS_PAGE_SIZE - 1u) / (size_t)NS_PAGE_SIZE;
    (void)ns_pmm_free_pages(pmm, physical, pages);
}

static uint64_t m5_clock_ms(void *context)
{
    (void)context;
    return arch_monotonic_ns() / M5_NANOSECONDS_PER_MILLISECOND;
}

static uint64_t m5_clock_ns(void *context)
{
    (void)context;
    return arch_monotonic_ns();
}

static void m5_relax(void *context)
{
    (void)context;
    arch_cpu_relax();
}

static uint64_t m5_irq_save(void *context)
{
    (void)context;
    return arch_irq_save();
}

static void m5_irq_restore(void *context, uint64_t flags)
{
    (void)context;
    arch_irq_restore(flags);
}

static void m5_pci_lock(void *context)
{
    (void)context;
    m5_pci_irq_flags = arch_irq_save();
    while (__atomic_test_and_set(&m5_pci_lock_word, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
}

static void m5_pci_unlock(void *context)
{
    uint64_t flags;
    (void)context;
    flags = m5_pci_irq_flags;
    __atomic_clear(&m5_pci_lock_word, __ATOMIC_RELEASE);
    arch_irq_restore(flags);
}

static const struct rtl8139_platform_ops m5_rtl_ops = {
    .in8 = m5_in8,
    .in16 = m5_in16,
    .in32 = m5_in32,
    .out8 = m5_out8,
    .out16 = m5_out16,
    .out32 = m5_out32,
    .dma_alloc = m5_dma_alloc,
    .dma_free = m5_dma_free,
    .monotonic_ms = m5_clock_ms,
    .cpu_relax = m5_relax,
    .irq_save = m5_irq_save,
    .irq_restore = m5_irq_restore,
    .pci_lock = m5_pci_lock,
    .pci_unlock = m5_pci_unlock,
};

static void m5_nic_interrupt(struct arch_interrupt_frame *frame, void *context)
{
    struct net_stack *stack = context;
    (void)frame;
    rtl8139_handle_interrupt(&stack->rtl8139);
}

static uint8_t m5_irq_vector(uint8_t irq)
{
    if (irq < 8u)
        return (uint8_t)(arch_pic_master_vector() + irq);
    return (uint8_t)(arch_pic_slave_vector() + irq - 8u);
}

static bool m5_wait_until(volatile uint32_t *condition, uint64_t timeout_ms)
{
    uint64_t start = m5_clock_ms(NULL);

    for (;;) {
        net_stack_poll(&m5_stack);
        if (__atomic_load_n(condition, __ATOMIC_ACQUIRE) != 0)
            return true;
        if (m5_clock_ms(NULL) - start >= timeout_ms)
            return false;
        arch_cpu_halt();
    }
}

static bool m5_wait_for_dhcp(uint64_t timeout_ms)
{
    uint64_t start = m5_clock_ms(NULL);

    for (;;) {
        net_stack_poll(&m5_stack);
        if (net_stack_dhcp_config(&m5_stack) != NULL)
            return true;
        if (m5_clock_ms(NULL) - start >= timeout_ms)
            return false;
        arch_cpu_halt();
    }
}

static int m5_socket_wait(void *context, uintptr_t backend_object,
                          uint32_t events, uint64_t deadline_ns)
{
    struct net_stack *stack = context;
    uint64_t flags;
    (void)backend_object;
    (void)events;

    net_stack_poll(stack);
    if (deadline_ns != NS_SOCKET_TIMEOUT_INFINITE &&
        arch_monotonic_ns() >= deadline_ns)
        return NS_SOCKET_ERR_TIMED_OUT;
    /* SYSCALL entry deliberately masks IF.  Blocking network operations must
     * admit the PIT and NIC interrupts while sleeping, then restore the
     * caller's interrupt state before returning through the syscall frame. */
    flags = arch_irq_save();
    arch_irq_enable();
    arch_cpu_halt();
    arch_irq_restore(flags);
    net_stack_poll(stack);
    return NS_SOCKET_OK;
}

static void m5_dns_callback(void *context,
                            const struct net_dns_result *result)
{
    (void)context;
    if (result != NULL)
        m5_dns_result = *result;
    __atomic_store_n(&m5_dns_complete, 1u, __ATOMIC_RELEASE);
}

static void m5_icmp_callback(net_device_t *device, net_ipv4_addr_t source,
                             uint16_t identifier, uint16_t sequence,
                             const uint8_t *payload, size_t payload_length,
                             void *context)
{
    (void)device;
    (void)context;
    if (net_ipv4_addr_to_u32(source) != NET_IPV4_ADDRESS(10, 0, 2, 2) ||
        identifier != m5_icmp_identifier || sequence != m5_icmp_sequence ||
        payload == NULL || payload_length > sizeof(m5_icmp_payload))
        return;
    memcpy(m5_icmp_payload, payload, payload_length);
    m5_icmp_payload_length = payload_length;
    __atomic_store_n(&m5_icmp_complete, 1u, __ATOMIC_RELEASE);
}

static void m5_verify_dns(void)
{
    enum net_dns_submit_result result;

    memset(&m5_dns_result, 0, sizeof(m5_dns_result));
    __atomic_store_n(&m5_dns_complete, 0u, __ATOMIC_RELEASE);
    result = net_dns_resolve(net_stack_dns(&m5_stack), "northstar.test",
                             m5_clock_ms(NULL), m5_dns_callback, NULL);
    if (result != NET_DNS_SUBMIT_STARTED &&
        result != NET_DNS_SUBMIT_COMPLETED_FROM_CACHE)
        m5_fail("DNS query could not be submitted");
    if (!m5_wait_until(&m5_dns_complete, M5_NETWORK_TIMEOUT_MS) ||
        m5_dns_result.status != NET_DNS_STATUS_OK ||
        m5_dns_result.address_count != 1u ||
        m5_dns_result.addresses[0] != NET_IPV4_ADDRESS(10, 0, 2, 2) ||
        strcmp(m5_dns_result.query_name, "northstar.test") != 0)
        m5_fail("DNS response failed validation");
    serial_write("# NS_TEST net.dns PASS\n");
}

static void m5_verify_icmp(void)
{
    static const uint8_t payload[] = "northstar-icmp";
    const struct net_dhcp_config *configuration =
        net_stack_dhcp_config(&m5_stack);

    if (configuration == NULL)
        m5_fail("ICMP test has no DHCP configuration");
    m5_icmp_identifier = 0x4e53u;
    m5_icmp_sequence = 1u;
    m5_icmp_payload_length = 0;
    __atomic_store_n(&m5_icmp_complete, 0u, __ATOMIC_RELEASE);
    net_icmp_set_echo_reply_handler(m5_icmp_callback, NULL);
    if (net_icmp_send_echo_request(
            net_ipv4_addr_from_u32(configuration->address),
            net_ipv4_addr_make(10, 0, 2, 2), m5_icmp_identifier,
            m5_icmp_sequence, payload, sizeof(payload) - 1u) != NET_OK ||
        !m5_wait_until(&m5_icmp_complete, M5_NETWORK_TIMEOUT_MS) ||
        m5_icmp_payload_length != sizeof(payload) - 1u ||
        memcmp(m5_icmp_payload, payload, sizeof(payload) - 1u) != 0)
        m5_fail("ICMP echo exchange failed");
    net_icmp_set_echo_reply_handler(NULL, NULL);
    serial_write("# NS_TEST net.icmp PASS\n");
}

static bool m5_robustness_observed(void)
{
    const struct ns_tcp_stats *tcp = &m5_stack.tcp.stats;
    const struct rtl8139_statistics *rtl =
        rtl8139_get_statistics(&m5_stack.rtl8139);

    return rtl != NULL && rtl->irq_count != 0 && rtl->rx_frames >= 8u &&
           rtl->tx_frames >= 8u &&
           m5_stack.device.stats.rx_dropped != 0 &&
           tcp->checksum_errors != 0 && tcp->retransmissions != 0 &&
           tcp->reordered_segments != 0 && tcp->duplicate_segments != 0;
}

static void m5_verify_robustness(void)
{
    uint64_t start = m5_clock_ms(NULL);
    const struct ns_tcp_stats *tcp;
    const struct rtl8139_statistics *rtl;

    while (!m5_robustness_observed()) {
        net_stack_poll(&m5_stack);
        if (m5_clock_ms(NULL) - start >= M5_NETWORK_TIMEOUT_MS)
            m5_fail("loss, reordering, duplicate, or malformed-frame evidence missing");
        arch_cpu_halt();
    }
    tcp = &m5_stack.tcp.stats;
    rtl = rtl8139_get_statistics(&m5_stack.rtl8139);
    serial_write("NS:NET:ROBUST irq=");
    write_decimal(rtl->irq_count);
    serial_write(" retransmit=");
    write_decimal(tcp->retransmissions);
    serial_write(" reorder=");
    write_decimal(tcp->reordered_segments);
    serial_write(" duplicate=");
    write_decimal(tcp->duplicate_segments);
    serial_write(" bad_checksum=");
    write_decimal(tcp->checksum_errors);
    serial_write(" dropped=");
    write_decimal(m5_stack.device.stats.rx_dropped);
    serial_putc('\n');
    serial_write("# NS_TEST net.robustness PASS\n");
}

/* Returns false only when the emulator did not expose an RTL8139 device. */
bool northstar_m5_run(void)
{
    const struct net_stack_config configuration = {
        .rtl8139_ops = &m5_rtl_ops,
        .rtl8139_platform_context = NULL,
        .clock_ms = m5_clock_ms,
        .clock_context = NULL,
        .dhcp_xid_seed = UINT32_C(0x4e535401),
        .tcp_sequence_seed = UINT32_C(0x4e535402),
        .dns_identifier_seed = UINT16_C(0x4e53),
    };
    struct ns_socket_config socket_configuration;
    const struct net_dhcp_config *dhcp;
    uint8_t irq;
    uint8_t vector;
    int result;

    arch_irq_disable();
    arch_timer_set_tick_handler(NULL, NULL);
    if (arch_pit_init(M5_PIT_HZ) == 0)
        m5_fail("could not start the network timer");
    m5_pci_lock_word = 0;
    result = net_stack_init(&m5_stack, &configuration);
    if (result == NET_ERR_NO_DEVICE) {
        arch_pit_shutdown();
        return false;
    }
    if (result != NET_OK)
        m5_fail("RTL8139 network stack initialization failed");
    irq = m5_stack.rtl8139.irq_line;
    if (irq >= ARCH_IRQ_COUNT)
        m5_fail("RTL8139 reported an invalid legacy IRQ");
    vector = m5_irq_vector(irq);
    if (!arch_interrupt_register(vector, m5_nic_interrupt, &m5_stack))
        m5_fail("could not register the RTL8139 interrupt");
    arch_pic_set_mask(irq, false);
    arch_irq_enable();

    serial_write("NS:NET:RTL8139 io=");
    write_decimal(m5_stack.rtl8139.io_base);
    serial_write(" irq=");
    write_decimal(irq);
    serial_putc('\n');
    serial_write("# NS_TEST net.rtl8139 PASS\n");

    if (net_stack_start_dhcp(&m5_stack) != NET_OK ||
        !m5_wait_for_dhcp(M5_NETWORK_TIMEOUT_MS))
        m5_fail("DHCP lease acquisition timed out");
    dhcp = net_stack_dhcp_config(&m5_stack);
    if (dhcp == NULL || dhcp->address != NET_IPV4_ADDRESS(10, 0, 2, 15) ||
        dhcp->subnet_mask != NET_IPV4_ADDRESS(255, 255, 255, 0) ||
        dhcp->router != NET_IPV4_ADDRESS(10, 0, 2, 2) ||
        dhcp->dns_server_count == 0 ||
        dhcp->dns_servers[0] != NET_IPV4_ADDRESS(10, 0, 2, 2))
        m5_fail("DHCP lease fields failed validation");
    serial_write("NS:NET:DHCP address=10.0.2.15 gateway=10.0.2.2\n");
    serial_write("# NS_TEST net.dhcp PASS\n");

    net_stack_socket_config(&m5_stack, m5_clock_ns, m5_socket_wait, &m5_stack,
                            &socket_configuration);
    ns_socket_table_init(&m5_sockets, &socket_configuration);
    m5_verify_dns();
    m5_verify_icmp();
    result = northstar_m3_run_netcheck(&m5_sockets);
    if (result != 0) {
        klog_hex("m5", "Ring-3 netcheck status=", (uint64_t)(int64_t)result);
        m5_fail("Ring-3 socket/HTTP program failed");
    }
    m5_verify_robustness();

    ns_socket_close_all(&m5_sockets);
    arch_irq_disable();
    arch_pic_set_mask(irq, true);
    (void)arch_interrupt_unregister(vector, m5_nic_interrupt, &m5_stack);
    net_stack_shutdown(&m5_stack);
    arch_pit_shutdown();
    serial_write("# NS_GATE G7 PASS\n");
    serial_write("NS:RUN:COMPLETE\n");
    kernel_debug_exit(0x10);
}
