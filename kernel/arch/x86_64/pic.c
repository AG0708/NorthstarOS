#include <northstar/arch_cpu.h>
#include <northstar/arch_io.h>
#include <northstar/arch_pic.h>

enum {
    PIC_MASTER_COMMAND = 0x20,
    PIC_MASTER_DATA = 0x21,
    PIC_SLAVE_COMMAND = 0xa0,
    PIC_SLAVE_DATA = 0xa1,
    PIC_EOI = 0x20,
    PIC_READ_ISR = 0x0b,
    PIC_ICW1_INIT = 0x10,
    PIC_ICW1_ICW4 = 0x01,
    PIC_ICW4_8086 = 0x01,
};

static uint8_t master_mask = 0xff;
static uint8_t slave_mask = 0xff;
static uint8_t master_offset = 32;
static uint8_t slave_offset = 40;

static uint8_t pic_read_isr(uint16_t command_port)
{
    arch_out8(command_port, PIC_READ_ISR);
    return arch_in8(command_port);
}

bool arch_pic_remap(uint8_t master_vector, uint8_t slave_vector)
{
    uint64_t flags;
    if ((master_vector & 7u) != 0 || (slave_vector & 7u) != 0 ||
        master_vector < 32 || slave_vector < 32 || master_vector > 248 ||
        slave_vector > 248 ||
        (master_vector < slave_vector + 8u &&
         slave_vector < master_vector + 8u))
        return false;

    flags = arch_irq_save();
    arch_out8(PIC_MASTER_COMMAND, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    arch_io_wait();
    arch_out8(PIC_SLAVE_COMMAND, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    arch_io_wait();
    arch_out8(PIC_MASTER_DATA, master_vector);
    arch_io_wait();
    arch_out8(PIC_SLAVE_DATA, slave_vector);
    arch_io_wait();
    arch_out8(PIC_MASTER_DATA, 1u << 2); /* slave wired to IRQ2 */
    arch_io_wait();
    arch_out8(PIC_SLAVE_DATA, 2u);       /* cascade identity */
    arch_io_wait();
    arch_out8(PIC_MASTER_DATA, PIC_ICW4_8086);
    arch_io_wait();
    arch_out8(PIC_SLAVE_DATA, PIC_ICW4_8086);
    arch_io_wait();

    master_offset = master_vector;
    slave_offset = slave_vector;
    arch_out8(PIC_MASTER_DATA, master_mask);
    arch_out8(PIC_SLAVE_DATA, slave_mask);
    arch_irq_restore(flags);
    return true;
}

void arch_pic_init(void)
{
    master_mask = 0xff;
    slave_mask = 0xff;
    (void)arch_pic_remap(32, 40);
}

void arch_pic_mask_all(void)
{
    uint64_t flags = arch_irq_save();
    master_mask = 0xff;
    slave_mask = 0xff;
    arch_out8(PIC_MASTER_DATA, master_mask);
    arch_out8(PIC_SLAVE_DATA, slave_mask);
    arch_irq_restore(flags);
}

void arch_pic_set_mask(uint8_t irq, bool masked)
{
    uint64_t flags;
    if (irq >= 16)
        return;

    flags = arch_irq_save();
    if (irq < 8) {
        if (masked)
            master_mask |= (uint8_t)(1u << irq);
        else
            master_mask &= (uint8_t)~(1u << irq);
    } else {
        uint8_t bit = (uint8_t)(1u << (irq - 8u));
        if (masked)
            slave_mask |= bit;
        else {
            slave_mask &= (uint8_t)~bit;
            master_mask &= (uint8_t)~(1u << 2);
        }
        if (slave_mask == 0xff)
            master_mask |= (uint8_t)(1u << 2);
    }
    arch_out8(PIC_MASTER_DATA, master_mask);
    arch_out8(PIC_SLAVE_DATA, slave_mask);
    arch_irq_restore(flags);
}

bool arch_pic_is_masked(uint8_t irq)
{
    if (irq >= 16)
        return true;
    if (irq < 8)
        return (master_mask & (1u << irq)) != 0;
    return (slave_mask & (1u << (irq - 8u))) != 0;
}

void arch_pic_eoi(uint8_t irq)
{
    if (irq >= 8)
        arch_out8(PIC_SLAVE_COMMAND, PIC_EOI);
    arch_out8(PIC_MASTER_COMMAND, PIC_EOI);
}

bool arch_pic_acknowledge_spurious(uint8_t irq)
{
    if (irq == 7) {
        return (pic_read_isr(PIC_MASTER_COMMAND) & (1u << 7)) == 0;
    }
    if (irq == 15 &&
        (pic_read_isr(PIC_SLAVE_COMMAND) & (1u << 7)) == 0) {
        /* The master accepted the cascade even though the slave withdrew it. */
        arch_out8(PIC_MASTER_COMMAND, PIC_EOI);
        return true;
    }
    return false;
}

uint8_t arch_pic_master_vector(void)
{
    return master_offset;
}

uint8_t arch_pic_slave_vector(void)
{
    return slave_offset;
}
