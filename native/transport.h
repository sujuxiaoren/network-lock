#pragma once
#include <stddef.h>
int diag_open(char *err, size_t errlen);
void diag_close(void);
int diag_exchange(const unsigned char *request, size_t req_len, unsigned char *response,
                  size_t response_cap, size_t *response_len, char *err, size_t errlen);
