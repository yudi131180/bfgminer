#ifndef BFG_DRIVER_OPENCL
#define BFG_DRIVER_OPENCL

#include <stdbool.h>

#include "miner.h"


extern char *print_ndevs_and_exit(int *ndevs);
extern void *reinit_gpu(void *userdata);
extern char *set_gpu_map(char *arg);
extern char *set_gpu_engine(char *arg);
extern char *set_gpu_fan(char *arg);
extern char *set_gpu_memclock(char *arg);
extern char *set_gpu_memdiff(char *arg);
extern char *set_gpu_powertune(char *arg);
extern char *set_gpu_vddc(char *arg);
extern char *set_temp_overheat(char *arg);
extern char *set_temp_target(char *arg);
extern char *set_intensity(char *arg);
extern char *set_vector(char *arg);
extern char *set_worksize(char *arg);
#ifdef USE_SCRYPT
extern char *set_shaders(char *arg);
extern char *set_lookup_gap(char *arg);
extern char *set_thread_concurrency(char *arg);
#endif
extern char *set_kernel(char *arg);
void manage_gpu(void);
extern void opencl_dynamic_cleanup();
extern void pause_dynamic_threads(int gpu);

extern bool have_opencl;
extern int opt_platform_id;
extern bool opt_opencl_binaries;

extern struct device_drv opencl_api;

#endif /* __DEVICE_GPU_H__ */
