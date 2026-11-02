/* tables test, main side: the exports tables.rs reads (no game code), a constructor for DT_INIT_ARRAY, and
 * data imported from the stand-in sdk (nn::util / nn::hid names only as symbol names) */
void ext_log(int);
int sdk_helper(int);
extern const unsigned char util_data[] __asm__("_ZN2nn4util9kFakeDataE");
extern const unsigned char npad_ids[] __asm__("_ZN2nn3hid6NpadId4FakeE");
extern const unsigned char os_data[] __asm__("_ZN2nn2os8kNotDataE");
extern const unsigned char big_data[] __asm__("_ZN2nn4util8kTooBigE");

int nninitStartup(void) { ext_log(2); return 0; }
static char heap[4096];
void *aligned_alloc(unsigned long align, unsigned long size) { return heap + (align & size & 63); }
void free(void *p) { ext_log(p != 0); }
void operator_delete(void *p) __asm__("_ZdlPv");
void operator_delete(void *p) { free(p); }
__attribute__((constructor)) static void ctor(void) { ext_log(3); }
int use_data(int i) { return util_data[i] + npad_ids[i] + os_data[i] + big_data[i] + sdk_helper(i); }
