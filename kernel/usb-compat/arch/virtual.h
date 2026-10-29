#include <stdint.h>
uintptr_t lp_virt_to_phys(const volatile void *p);
void *lp_phys_to_virt(uintptr_t p);
#define virt_to_phys lp_virt_to_phys
#define phys_to_virt lp_phys_to_virt
#define virt_to_bus lp_virt_to_phys
#define bus_to_virt lp_phys_to_virt
