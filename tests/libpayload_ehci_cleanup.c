#include "../kernel/third_party/libpayload/drivers/usb/ehci.c"

void test_ehci_cleanup(unsigned count, int nonzero_terminated_link)
{
    ehci_qh_t *qh = dma_memalign(64, sizeof(*qh));
    qtd_t *head = NULL;
    qtd_t *tail = NULL;
    qtd_t *ignored = NULL;
    if (!qh) __builtin_trap();
    for (unsigned i = 0; i < count; i++) {
        qtd_t *td = dma_memalign(64, sizeof(*td));
        if (!td) __builtin_trap();
        memset((void *)td, 0, sizeof(*td));
        if (tail) tail->next_qtd = virt_to_phys(td);
        else head = td;
        tail = td;
    }
    if (nonzero_terminated_link) {
        ignored = dma_memalign(64, sizeof(*ignored));
        if (!ignored) __builtin_trap();
        memset((void *)ignored, 0, sizeof(*ignored));
        ignored->next_qtd = QTD_TERMINATE;
    }
    if (tail) tail->next_qtd = QTD_TERMINATE | virt_to_phys(ignored);
    free_qh_and_tds(qh, head);
    free((void *)ignored);
}
