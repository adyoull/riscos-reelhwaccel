/* fake_vcenc_lib.c's interface: the fake VCHIQ module and MMAL encoder as a library */
#ifndef FAKE_VCENC_H
#define FAKE_VCENC_H
#include <stdint.h>
void fake_vcenc_reset(void);               /* a fresh fake */
int fake_vcenc_fails(void);                /* the fake's own CHECK failures so far */
int fake_vcenc_cleaned(const char *what);  /* everything closed and freed? */
int *fake_vcenc_var(const char *name);     /* a scenario switch or counter */
long long fake_vcenc_param(uint32_t id, int i);   /* value i of the last setting of parameter id, or -1 */
uint32_t fake_vcenc_adler(int k);          /* the Adler-32 of picture k as the encoder read it */
#endif
