KERNEL_C_SRCS += kernel/net/socket_api.c

ifeq ($(NORTHSTAR_ENABLE_M5),1)
KERNEL_C_SRCS += \
	kernel/core/m5.c \
	kernel/drivers/rtl8139.c \
	kernel/net/net_checksum.c \
	kernel/net/net_device.c \
	kernel/net/net_ethernet.c \
	kernel/net/net_arp.c \
	kernel/net/net_ipv4.c \
	kernel/net/net_icmp.c \
	kernel/net/net_udp.c \
	kernel/net/net_tcp.c \
	kernel/net/net_dhcp.c \
	kernel/net/net_dns.c \
	kernel/net/socket_net_backend.c \
	kernel/net/net_stack.c
endif

HOST_TEST_SRCS += \
	tests/host/test_net_base.c \
	tests/host/test_net_udp_socket.c \
	tests/host/test_net_tcp.c \
	tests/host/test_net_services.c \
	tests/host/test_net_rtl8139.c \
	tests/host/test_net_stack.c \
	tests/host/test_net_socket_backend.c

.PHONY: networking-python-tests
networking-python-tests:
	python3 tests/host/test_net_peer.py
