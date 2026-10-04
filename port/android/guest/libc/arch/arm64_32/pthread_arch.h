#ifdef HALO_SWITCH
/* On the Switch the guest's thread pointer is tpidr_el0, which the kernel
keeps for each thread and libnx leaves alone (its own is tpidrro_el0):
guest/runtime/guest_thread.c sets it. (volatile: else a read from before
it is set, in code inlined with the setting, could stand in for one after) */
static inline uintptr_t __get_tp()
{
	unsigned long long tp;

	__asm__ volatile ("mrs %0, tpidr_el0" : "=r"(tp));
	return (uintptr_t)tp;
}
#else
/* The guest has no thread pointer register of its own (tpidr_el0 belongs
to the host's bionic), so the runtime hands out each thread's struct pthread
(guest/runtime/guest_thread.c). */
uintptr_t __guest_get_tp(void);

static inline uintptr_t __get_tp()
{
	return __guest_get_tp();
}
#endif

#define MC_PC pc
