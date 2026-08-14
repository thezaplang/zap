#include <stdint.h>

extern int64_t retained_c_api(void);

void *zap_arc_default_context(void) { return 0; }

void zap_arc_collect_at_safepoint(void *context) { (void)context; }

int main(void) { return retained_c_api() == 42 ? 0 : 1; }
