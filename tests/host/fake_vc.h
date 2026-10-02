/* fake_vc.c's interface: the fake VCHIQ module and MMAL decoder as a library */
#ifndef FAKE_VC_H
#define FAKE_VC_H
void fake_vc_reset(void);                  /* a fresh fake, MP4 mode (access units with pts) */
void fake_vc_make_mp4(void);               /* /tmp/mmaldecode_test.mp4 (12 pictures) */
int fake_vc_fails(void);                   /* the fake's own CHECK failures so far */
int fake_vc_cleaned(const char *what);     /* everything closed and freed? */
int *fake_vc_var(const char *name);        /* a scenario switch or counter */
int fake_vc_width(void);
int fake_vc_height(void);
int fake_vc_value(int k);                  /* picture k's Y value */
#endif
