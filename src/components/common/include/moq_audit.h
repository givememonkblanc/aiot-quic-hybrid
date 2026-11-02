#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool moq_audit_init(void);
int moq_audit_printf(const char *format, ...) __attribute__((format(printf,1,2)));
bool moq_audit_export(void);
#ifdef __cplusplus
}
#endif
