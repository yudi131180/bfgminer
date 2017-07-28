#ifndef BFG_OCL_H
#define BFG_OCL_H

#include "config.h"

#include <stdbool.h>
#ifdef HAVE_OPENCL
#include "CL/cl.h"

#include "miner.h"

typedef struct {
	cl_context context;
	cl_kernel kernel;
	cl_command_queue commandQueue;
	cl_program program;
	cl_mem outputBuffer;
#ifdef USE_SCRYPT
	cl_mem CLbuffer0;
	cl_mem padbuffer8;
	size_t padbufsize;
	void * cldata;
#endif
	bool hasBitAlign;
	bool hasOpenCL11plus;
	bool goffset;
	cl_uint vwidth;
	size_t max_work_size;
	size_t wsize;
	enum cl_kernels chosen_kernel;
} _clState;

extern char *file_contents(const char *filename, int *length);
extern int clDevicesNum(void);
extern _clState *initCl(unsigned int gpu, char *name, size_t nameSize);
#endif /* HAVE_OPENCL */
#endif /* __OCL_H__ */
