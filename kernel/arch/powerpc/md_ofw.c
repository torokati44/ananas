#include <ananas/types.h>
#include <ananas/lib.h>
#include <machine/frame.h>
#include <machine/macro.h>
#include <machine/vm.h>
#include <ofw.h>

static uint32_t ofw_msr;
static uint32_t ofw_sprg[4];
static ofw_entry_t ofw_entry;

static uint32_t orig_msr;
static uint32_t orig_sprg0;
static uint32_t orig_sr[PPC_NUM_SREGS];

int
ofw_call(void* arg)
{
	int retval;

	/* Save our MSR and restore the OFW one */
	orig_msr = rdmsr();
	wrmsr(ofw_msr);

	/* Save our SPRG's - we only care about %sprg0 for now */
	__asm __volatile("mfsprg0 %0" : "=r" (orig_sprg0));

	/* FIXME The following causes OFW to hang... need to investigate why */
#ifdef NOTYET
	/* ...and restore the OFW ones */
	__asm __volatile(
		"mtsprg0 %0\n"
		"mtsprg1 %1\n"
		"mtsprg2 %2\n"
		"mtsprg3 %3\n"
	: : "r" (ofw_sprg[0]), "r" (ofw_sprg[1]),
	    "r" (ofw_sprg[2]), "r" (ofw_sprg[3]));
#endif
	__asm __volatile("isync");

	retval = ofw_entry(arg);

	/* Restore our SPRG */
	__asm __volatile("mtsprg0 %0" : : "r" (orig_sprg0));

	/* And restore our MSR */
	wrmsr(orig_msr);

	return retval;
}

void
ofw_md_init(register_t entry)
{
	/*
	 * Save the OpenFirmware entry point and machine registers; it'll cry and
	 * tumble if we do not restore them.
	 */
	ofw_msr = rdmsr();
	__asm __volatile("mfsprg0 %0" : "=r" (ofw_sprg[0]));
	__asm __volatile("mfsprg1 %0" : "=r" (ofw_sprg[1]));
	__asm __volatile("mfsprg2 %0" : "=r" (ofw_sprg[2]));
	__asm __volatile("mfsprg3 %0" : "=r" (ofw_sprg[3]));
	ofw_entry = (ofw_entry_t)entry;

	/* Initial OpenFirmware I/O, this will make kprintf() work */
	ofw_init_io();
	TRACE("OFW entry point is 0x%x\n", ofw_entry);

	/* XXX As of now, there's no support for real-mode mapped OpenFirmware */
	if ((ofw_msr & (MSR_DR | MSR_IR)) != (MSR_DR | MSR_IR))
		panic("OpenFirmware isn't page-mapped");
}

/* vim:set ts=2 sw=2: */
