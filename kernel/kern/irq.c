#include "machine/interrupts.h"
#include "irq.h"
#include "lib.h"

static struct IRQ irq[MAX_IRQS];

void
irq_init()
{
	memset(irq, 0, sizeof(struct IRQ) * MAX_IRQS);
}

int
irq_register(unsigned int no, device_t dev, irqhandler_t handler)
{
	KASSERT(no < MAX_IRQS, "interrupt %u out of range", no);

	if (irq[no].handler != NULL)
		return 0;

	irq[no].dev = dev;
	irq[no].handler = handler;
	return 1;
}

void
irq_handler(unsigned int no)
{
	if (no >= MAX_IRQS)
		panic("irq_handler: impossible irq %u fired", no);

	if (irq[no].handler == NULL) {
		kprintf("irq_handler(): unhandled irq %u, ignored\n", no);
		return;
	}

	irq[no].handler(irq[no].dev);
}

/* vim:set ts=2 sw=2: */