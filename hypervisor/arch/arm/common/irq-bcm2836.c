/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/minos.h>
#include <asm/io.h>
#include <minos/percpu.h>
#include <minos/spinlock.h>
#include <minos/print.h>
#include <minos/errno.h>
#include <minos/vmodule.h>
#include <minos/vcpu.h>
#include <asm/arch.h>
#include <minos/cpumask.h>
#include <minos/irq.h>
#include <minos/virq.h>
#include <asm/of.h>
#include <asm/bcm_irq.h>

extern int bcm_virq_init(unsigned long l1_base, size_t l1_size,
		unsigned long l2_base, size_t l2_size);

static const int reg_pending[] = { 0x00, 0x04, 0x08 };
static const int reg_enable[] = { 0x18, 0x10, 0x14 };
static const int reg_disable[]= { 0x24, 0x1c, 0x20 };

static const int shortcuts[] = {
	7, 9, 10, 18, 19,		/* Bank 1 */
	21, 22, 23, 24, 25, 30		/* Bank 2 */
};

struct armctrl_ic {
	void *base;
	void *pending[NR_BANKS];
	void *enable[NR_BANKS];
	void *disable[NR_BANKS];
	struct irq_domain *domain;
	void *local_base;
};

static struct armctrl_ic intc;
static void *bcm2836_base;

static void bcm2835_mask_irq(uint32_t irq)
{
	writel_relaxed(HWIRQ_BIT(irq), intc.disable[HWIRQ_BANK(irq)]);
	dsb();
}

static void bcm2835_unmask_irq(uint32_t irq)
{
	writel_relaxed(HWIRQ_BIT(irq),
			       intc.enable[HWIRQ_BANK(irq)]);
	dsb();
}

static uint32_t armctrl_translate_bank(int bank)
{
	uint32_t stat = readl_relaxed(intc.pending[bank]);

	return MAKE_HWIRQ(bank, __ffs(stat));
}

static uint32_t armctrl_translate_shortcut(int bank, u32 stat)
{
	return MAKE_HWIRQ(bank, shortcuts[__ffs(stat >> SHORTCUT_SHIFT)]);
}

static uint32_t bcm2835_get_pending(void)
{
	uint32_t stat = readl_relaxed(intc.pending[0]) & BANK0_VALID_MASK;

	if (stat == 0)
		return BAD_IRQ;
	else if (stat & BANK0_HWIRQ_MASK)
		return MAKE_HWIRQ(0, __ffs(stat & BANK0_HWIRQ_MASK));
	else if (stat & SHORTCUT1_MASK)
		return armctrl_translate_shortcut(1, stat & SHORTCUT1_MASK);
	else if (stat & SHORTCUT2_MASK)
		return armctrl_translate_shortcut(2, stat & SHORTCUT2_MASK);
	else if (stat & BANK1_HWIRQ)
		return armctrl_translate_bank(1);
	else if (stat & BANK2_HWIRQ)
		return armctrl_translate_bank(2);
	else
		BUG();
}

static uint32_t bcm2836_get_pending(void)
{
	int cpu = smp_processor_id();
	uint32_t stat;
	uint32_t irq;

	stat = readl_relaxed(bcm2836_base + LOCAL_IRQ_PENDING0 + 4 * cpu);

	if (stat & BIT(LOCAL_IRQ_MAILBOX0)) {
		void *mailbox0;
		uint32_t mbox_val;

		/*
		 * support 32 IPI, here only use 16 bit to routing
		 * to the sgi interrupt
		 */
		mailbox0 = bcm2836_base + LOCAL_MAILBOX0_CLR0 + 16 * cpu;
		mbox_val = readl_relaxed(mailbox0);
		if (mbox_val == 0)
			return BAD_IRQ;

		irq = __ffs(mbox_val);
		writel_relaxed(1 << irq, mailbox0);
		dsb();

		if (irq >= 16)
			return BAD_IRQ;

		return irq;
	} else if (stat) {
		/*
		 * map other irq except mailbox to PPI as below:
		 * 16	: CNTPSIRQ
		 * 17	: CNTPNSIRQ
		 * 18	: CNTHPIRQ
		 * 19	: CNTVIRQ
		 * 20 - 23 : Mailbox irq
		 * 24	: GPU interrupt
		 * 25	: PMU interrupt
		 * 26	: AXI outstanding interrupt
		 * 27	: Local timer interrupt
		 */
		irq = __ffs(stat) + 16;
		return irq;
	}

	return BAD_IRQ;
}

static void bcm2836_mask_per_cpu_irq(unsigned int reg_offset,
		unsigned int bit, int cpu)
{
	void *reg = bcm2836_base + reg_offset + 4 * cpu;

	writel_relaxed(readl_relaxed(reg) & ~BIT(bit), reg);
	dsb();
}

static void bcm2836_unmask_per_cpu_irq(unsigned int reg_offset,
		unsigned int bit, int cpu)
{
	void *reg = bcm2836_base + reg_offset + 4 * cpu;

	writel_relaxed(readl_relaxed(reg) | BIT(bit), reg);
	dsb();
}

static void inline __bcm2836_mask_irq(uint32_t irq, int cpu)
{
	int offset;

	if (irq >= 32)
		bcm2835_mask_irq(irq);

	/* TBD : sgi always enable */
	if (irq < 16)
		return;

	offset = irq - 16;
	switch (offset) {
	case LOCAL_IRQ_CNTPSIRQ:
	case LOCAL_IRQ_CNTPNSIRQ:
	case LOCAL_IRQ_CNTHPIRQ:
	case LOCAL_IRQ_CNTVIRQ:
		bcm2836_mask_per_cpu_irq(LOCAL_TIMER_INT_CONTROL0, offset, cpu);
		break;

	case LOCAL_IRQ_PMU_FAST:
		writel_relaxed(1 << cpu, bcm2836_base + LOCAL_PM_ROUTING_CLR);
		dsb();
		break;

	case LOCAL_IRQ_GPU_FAST:
		break;
	}
}

static void inline __bcm2836_unmask_irq(uint32_t irq, int cpu)
{
	int offset;

	if (irq >= 32)
		bcm2835_unmask_irq(irq);

	if (irq < 16)
		return;

	offset = irq - 16;
	switch (offset) {
	case LOCAL_IRQ_CNTPSIRQ:
	case LOCAL_IRQ_CNTPNSIRQ:
	case LOCAL_IRQ_CNTHPIRQ:
	case LOCAL_IRQ_CNTVIRQ:
		bcm2836_unmask_per_cpu_irq(LOCAL_TIMER_INT_CONTROL0, offset, cpu);
		break;

	case LOCAL_IRQ_PMU_FAST:
		writel_relaxed(1 << cpu, bcm2836_base + LOCAL_PM_ROUTING_SET);
		dsb();
		break;

	case LOCAL_IRQ_GPU_FAST:
		break;
	}
}

static int bcm2836_set_irq_priority(uint32_t irq, uint32_t pr)
{
	return 0;
}

static void bcm2836_mask_irq(uint32_t irq)
{
	__bcm2836_mask_irq(irq, smp_processor_id());
}

static void bcm2836_unmask_irq(uint32_t irq)
{
	__bcm2836_unmask_irq(irq, smp_processor_id());
}

static void bcm2836_mask_irq_cpu(uint32_t irq, int cpu)
{
	__bcm2836_mask_irq(irq, cpu);
}

static void bcm2836_unmask_irq_cpu(uint32_t irq, int cpu)
{
	__bcm2836_unmask_irq(irq, cpu);
}

static int bcm2836_get_virq_state(struct vcpu *vcpu, struct virq_desc *virq)
{
	return 0;
}

static int bcm2836_get_virq_nr(void)
{
	/* support max 128 virqs */

	return 128;
}

static void bcm2836_send_sgi(uint32_t sgi, enum sgi_mode mode, cpumask_t *cpu)
{
	int c;
	void *mailbox0_base = bcm2836_base + LOCAL_MAILBOX0_SET0;

	dsb();
	if (sgi >= 16)
		return;

	switch (mode) {
	case SGI_TO_OTHERS:
		for_each_cpu(c, cpu) {
			if (c == smp_processor_id())
				continue;
			writel_relaxed(1 << sgi, mailbox0_base + 16 * c);
			dsb();
		}
		break;
	case SGI_TO_SELF:
		writel_relaxed(1 << sgi, mailbox0_base +
				16 * smp_processor_id());
		dsb();
		break;
	case SGI_TO_LIST:
		for_each_cpu(c, cpu) {
			writel_relaxed(1 << sgi, mailbox0_base + 16 * c);
			dsb();
		}
		break;
	}
}

static int bcm2836_update_virq(struct vcpu *vcpu,
		struct virq_desc *desc, int action)
{
	switch (action) {
	case VIRQ_ACTION_CLEAR:
		/* enable the hardware irq when disable in hyp */
		if ((desc->vno >= 32) && (virq_is_hw(desc)))
			bcm2836_unmask_irq(desc->hno);
		break;
	}
	return 0;
}

static int bcm2836_send_virq(struct vcpu *vcpu, struct virq_desc *virq)
{
	/*
	 * if the hardware platform is bcm2836 such
	 * as rpi3b/rpi3b+, the native vm is using the
	 * original bcm2836 virq controller, but the
	 * guest vm will use vgicv2, vgicv2 can send
	 * the virq when the mmio emulated
	 */
	if (vm_is_native(vcpu->vm))
		bcm_virq_send_virq(vcpu, virq->vno);

	return 0;
}

static int bcm2836_set_irq_affinity(uint32_t irq, uint32_t pcpu)
{
	return 0;
}

static int bcm2836_set_irq_type(uint32_t irq, uint32_t type)
{
	return 0;
}

static void bcm2836_dir_irq(uint32_t irq)
{
	if (irq >= 32)
		bcm2835_unmask_irq(irq);
}

static void bcm2836_eoi_irq(uint32_t irq)
{
	if (irq >= 32)
		bcm2835_mask_irq(irq);
}

static int bcm2836_irq_enter_to_guest(void *item, void *data)
{
	unsigned long flags;
	struct virq_desc *virq, *n;
	struct vcpu *vcpu = (struct vcpu *)item;
	struct virq_struct *virq_struct = vcpu->virq_struct;

	/*
	 * if there is no pending virq for this vcpu
	 * clear the virq state in HCR_EL2 then just return
	 * else inject the virq
	 */
	spin_lock_irqsave(&virq_struct->lock, flags);

	if (is_list_empty(&virq_struct->pending_list) &&
			is_list_empty(&virq_struct->active_list)) {
		arch_clear_virq_flag();
		goto out;
	}

	arch_set_virq_flag();

	/*
	 * for vgicv2 do not need to update the pending
	 * irq here
	 */
	if (!vm_is_native(vcpu->vm))
		goto out;

	/*
	 * just inject one virq the time since when the
	 * virq is handled, the vm will trap to hypervisor
	 * again, actually here can inject all virq to the
	 * guest, but it is hard to judge whether all virq
	 * has been handled by guest vm TBD
	 */
	list_for_each_entry_safe(virq, n, &virq_struct->pending_list, list) {
		if (!virq_is_pending(virq)) {
			pr_error("virq is not request %d\n", virq->vno);
			list_del(&virq->list);
			virq->list.next = NULL;
			continue;
		}

#if 0
		/*
		 * virq is not enabled this time, need to
		 * send it later, but this will infence the idle
		 * condition jugement TBD
		 */
		if (!virq_is_enabled(virq))
			continue;
#endif

		/*
		 * update the bcm_virq interrupt status and
		 * delete the virq from the virq list then
		 * add it to active list
		 */
		bcm2836_send_virq(vcpu, virq);
		virq_clear_pending(virq);
		virq->state = VIRQ_STATE_ACTIVE;
		list_del(&virq->list);
		list_add_tail(&virq_struct->active_list, &virq->list);
	}

out:
	spin_unlock_irqrestore(&virq_struct->lock, flags);

	return 0;
}

int bcm2835_irq_handler(uint32_t irq, void *data)
{
	uint32_t no;
	struct irq_desc *irq_desc;

	while ((no = bcm2835_get_pending()) != BAD_IRQ) {
		irq_desc = get_irq_desc(no);
		if (!irq_desc || !irq_desc->handler) {
			bcm2835_mask_irq(no);
			pr_error("irq is not register disable it\n", irq);
			continue;
		}

		irq_desc->handler(irq_desc->hno, irq_desc->pdata);

		/*
		 * if the hardware irq is for vm mask it here
		 * until the vm notify that the hardware irq
		 * is handled
		 */
		if (test_bit(IRQ_FLAGS_VCPU_BIT, &irq_desc->flags))
			bcm2835_mask_irq(no);
	}

	return 0;
}

static int bcm2836_irq_init(int node)
{
	void *base;
	int b;

	pr_info("boardcom bcm2836 l1 interrupt init\n");

	bcm2836_base = (void *)0x40000000;
	io_remap(0x40000000, 0x40000000, 0x100);

	/*
	 * set the timer to source for the 19.2Mhz crstal clock
	 * and set the timer prescaler to 1:1
	 */
	writel_relaxed(0, bcm2836_base + LOCAL_CONTROL);
	writel_relaxed(0x80000000, bcm2836_base + LOCAL_PRESCALER);

	/*
	 * int rpi-3b there are two irq_chip controller, the
	 * bcm2836 local interrupt controller is percpu and
	 * the bcm2835 is not percpu so :
	 * bcm2836 id  : 0 - 31
	 * bcm2835 id  : 32 - 127
	 */
	irq_alloc_sgi(0, 16);
	irq_alloc_ppi(16, 16);

	/* enable mailbox0 interrupt for each core */
	writel_relaxed(1, bcm2836_base + LOCAL_MAILBOX_INT_CONTROL0);
	writel_relaxed(1, bcm2836_base + LOCAL_MAILBOX_INT_CONTROL0 + 0x4);
	writel_relaxed(1, bcm2836_base + LOCAL_MAILBOX_INT_CONTROL0 + 0x8);
	writel_relaxed(1, bcm2836_base + LOCAL_MAILBOX_INT_CONTROL0 + 0xc);

	/* init the bcm2835 interrupt controller for spi */
	base = intc.base = (void *)0x3f00b200;
	io_remap(0x3f00b200, 0x3f00b200, 0x100);

	for (b = 0; b < NR_BANKS; b++) {
		intc.pending[b] = base + reg_pending[b];
		intc.enable[b] = base + reg_enable[b];
		intc.disable[b] = base + reg_disable[b];
	}

	irq_alloc_spi(32, 96);

	/* init the virq device callback */
	bcm_virq_init(0x40000000, 0x100, 0x3f00b200, 0x1000);

	register_hook(bcm2836_irq_enter_to_guest,
			MINOS_HOOK_TYPE_ENTER_TO_GUEST);

	/*
	 * request the irq handler for the bcm2835 inc
	 * TBD - now the hardware irq only route to cpu0
	 */
	request_irq(24, bcm2835_irq_handler, 0, "bcm2835_irq", NULL);

	return 0;
}

static int bcm2836_secondary_init(void)
{
	return 0;
}

static struct irq_chip bcm2836_irq_chip = {
	.irq_mask		= bcm2836_mask_irq,
	.irq_mask_cpu		= bcm2836_mask_irq_cpu,
	.irq_unmask		= bcm2836_unmask_irq,
	.irq_unmask_cpu		= bcm2836_unmask_irq_cpu,
	.irq_eoi		= bcm2836_eoi_irq,
	.irq_dir		= bcm2836_dir_irq,
	.irq_set_type		= bcm2836_set_irq_type,
	.get_pending_irq	= bcm2836_get_pending,
	.irq_set_affinity 	= bcm2836_set_irq_affinity,
	.send_sgi		= bcm2836_send_sgi,
	.irq_set_priority	= bcm2836_set_irq_priority,
	.get_virq_state		= bcm2836_get_virq_state,
	.send_virq		= bcm2836_send_virq,
	.update_virq		= bcm2836_update_virq,
	.get_virq_nr		= bcm2836_get_virq_nr,
	.init			= bcm2836_irq_init,
	.secondary_init		= bcm2836_secondary_init,
};

IRQCHIP_DECLARE(bcm2836_chip, "brcm,bcm2836-l1-intc",
		(void *)&bcm2836_irq_chip);
