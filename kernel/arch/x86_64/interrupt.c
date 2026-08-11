#include <northstar/arch_cpu.h>
#include <northstar/arch_gdt.h>
#include <northstar/arch_interrupt.h>
#include <northstar/arch_pic.h>
#include <northstar/kernel.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} NS_PACKED;

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} NS_PACKED;

struct handler_slot {
    arch_interrupt_handler_t handler;
    void *context;
};

NS_STATIC_ASSERT(sizeof(struct idt_entry) == 16,
                 "x86-64 IDT entry must be 16 bytes");
NS_STATIC_ASSERT(sizeof(struct idt_pointer) == 10,
                 "IDTR image must be 10 bytes");
NS_STATIC_ASSERT(offsetof(struct arch_interrupt_frame, vector) == 120,
                 "ISR vector offset changed");
NS_STATIC_ASSERT(offsetof(struct arch_interrupt_frame, error_code) == 128,
                 "ISR error-code offset changed");
NS_STATIC_ASSERT(offsetof(struct arch_interrupt_frame, rip) == 136,
                 "ISR RIP offset changed");
NS_STATIC_ASSERT(offsetof(struct arch_interrupt_frame, rsp) == 160,
                 "ISR user-RSP offset changed");
NS_STATIC_ASSERT(sizeof(struct arch_interrupt_frame) == 176,
                 "ISR frame assembly ABI changed");

extern const uintptr_t arch_isr_stub_table[ARCH_INTERRUPT_VECTOR_COUNT];

static struct idt_entry idt[ARCH_INTERRUPT_VECTOR_COUNT] NS_ALIGNED(16);
static struct handler_slot handlers[ARCH_INTERRUPT_VECTOR_COUNT];
static uint64_t counts[ARCH_INTERRUPT_VECTOR_COUNT];

static const char *const exception_names[ARCH_EXCEPTION_COUNT] = {
    "divide error", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "bound range exceeded", "invalid opcode",
    "device not available", "double fault", "coprocessor overrun",
    "invalid TSS", "segment not present", "stack-segment fault",
    "general protection fault", "page fault", "reserved",
    "x87 floating-point exception", "alignment check", "machine check",
    "SIMD floating-point exception", "virtualization exception",
    "control-protection exception", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "hypervisor injection exception",
    "VMM communication exception", "security exception", "reserved",
};

static void idt_set(uint8_t vector, uintptr_t address, uint8_t ist,
                    uint8_t attributes)
{
    struct idt_entry *entry = &idt[vector];
    entry->offset_low = (uint16_t)address;
    entry->selector = NS_GDT_KERNEL_CODE;
    entry->ist = ist & 7u;
    entry->attributes = attributes;
    entry->offset_middle = (uint16_t)(address >> 16);
    entry->offset_high = (uint32_t)(address >> 32);
    entry->reserved = 0;
}

void arch_interrupt_init(void)
{
    struct idt_pointer pointer;
    unsigned vector;
    uint64_t flags = arch_irq_save();

    memset(idt, 0, sizeof(idt));
    memset(handlers, 0, sizeof(handlers));
    memset(counts, 0, sizeof(counts));

    for (vector = 0; vector < ARCH_INTERRUPT_VECTOR_COUNT; ++vector) {
        uint8_t ist = 0;
        uint8_t attributes = 0x8e; /* present ring-0 interrupt gate */
        if (vector == 2)
            ist = 1; /* NMI */
        else if (vector == 8)
            ist = 2; /* double fault */
        else if (vector == 18)
            ist = 3; /* machine check */
        if (vector == 3 || vector == 4)
            attributes = 0xef; /* ring-3 trap gates */
        else if (vector == ARCH_SYSCALL_VECTOR)
            attributes = 0xee; /* ring-3 interrupt gate */
        idt_set((uint8_t)vector, arch_isr_stub_table[vector], ist,
                attributes);
    }

    pointer.limit = sizeof(idt) - 1u;
    pointer.base = (uintptr_t)idt;
    __asm__ volatile("lidt %0" : : "m"(pointer) : "memory");
    arch_irq_restore(flags);
}

bool arch_interrupt_register(uint8_t vector, arch_interrupt_handler_t handler,
                             void *context)
{
    uint64_t flags;
    bool installed = false;
    if (handler == NULL)
        return false;

    flags = arch_irq_save();
    if (handlers[vector].handler == NULL) {
        handlers[vector].context = context;
        __atomic_store_n(&handlers[vector].handler, handler, __ATOMIC_RELEASE);
        installed = true;
    }
    arch_irq_restore(flags);
    return installed;
}

bool arch_interrupt_unregister(uint8_t vector,
                               arch_interrupt_handler_t handler,
                               void *context)
{
    uint64_t flags;
    bool removed = false;
    flags = arch_irq_save();
    if (handlers[vector].handler == handler &&
        handlers[vector].context == context) {
        __atomic_store_n(&handlers[vector].handler, NULL, __ATOMIC_RELEASE);
        handlers[vector].context = NULL;
        removed = true;
    }
    arch_irq_restore(flags);
    return removed;
}

uint64_t arch_interrupt_count(uint8_t vector)
{
    return __atomic_load_n(&counts[vector], __ATOMIC_RELAXED);
}

const char *arch_exception_name(uint8_t vector)
{
    if (vector >= ARCH_EXCEPTION_COUNT)
        return "not an exception";
    return exception_names[vector];
}

static bool interrupt_to_irq(uint8_t vector, uint8_t *irq)
{
    uint8_t master = arch_pic_master_vector();
    uint8_t slave = arch_pic_slave_vector();
    if (vector >= master && vector < (uint8_t)(master + 8u)) {
        *irq = vector - master;
        return true;
    }
    if (vector >= slave && vector < (uint8_t)(slave + 8u)) {
        *irq = (uint8_t)(8u + vector - slave);
        return true;
    }
    return false;
}

static void unhandled_exception(const struct arch_interrupt_frame *frame)
{
    klog("exception", arch_exception_name((uint8_t)frame->vector));
    klog_hex("exception", "vector=", frame->vector);
    klog_hex("exception", "error=", frame->error_code);
    klog_hex("exception", "rip=", frame->rip);
    klog_hex("exception", "cs=", frame->cs);
    klog_hex("exception", "rflags=", frame->rflags);
    if (frame->vector == 14)
        klog_hex("exception", "cr2=", arch_read_cr2());
    panic("unhandled processor exception");
}

void arch_interrupt_dispatch(struct arch_interrupt_frame *frame)
{
    arch_interrupt_handler_t handler;
    void *context;
    uint8_t vector;
    uint8_t irq;

    if (frame == NULL || frame->vector >= ARCH_INTERRUPT_VECTOR_COUNT)
        panic("corrupt interrupt frame");
    vector = (uint8_t)frame->vector;
    __atomic_add_fetch(&counts[vector], 1, __ATOMIC_RELAXED);

    if (interrupt_to_irq(vector, &irq)) {
        if (arch_pic_acknowledge_spurious(irq))
            return;
        /* All legacy PIC inputs used by NorthstarOS are edge-triggered.  EOI
         * before invoking the handler is necessary because the timer handler
         * may context-switch and not return on this stack until much later. */
        arch_pic_eoi(irq);
    }

    handler = __atomic_load_n(&handlers[vector].handler, __ATOMIC_ACQUIRE);
    context = handlers[vector].context;
    if (handler != NULL)
        handler(frame, context);
    else if (vector < ARCH_EXCEPTION_COUNT)
        unhandled_exception(frame);

}
