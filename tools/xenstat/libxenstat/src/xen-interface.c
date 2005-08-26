/* xen-interface.c
 *
 * Copyright (C) International Business Machines Corp., 2005
 * Authors: Josh Triplett <josht@us.ibm.com>
 *          Judy Fischbach <jfisch@us.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 */

#include "xen-interface.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "version.h"
#include "privcmd.h"
#include "xen.h"

struct xi_handle {
	int fd;
};

/* Initialize for xen-interface.  Returns a handle to be used with subsequent
 * calls to the xen-interface functions or NULL if an error occurs. */
xi_handle *xi_init()
{
	xi_handle *handle;

	handle = (xi_handle *)calloc(1, sizeof(xi_handle));
	if (handle == NULL)
		return NULL;

	handle->fd = open("/proc/xen/privcmd", O_RDWR);
	if (handle->fd < 0) {
		perror("Couldn't open /proc/xen/privcmd");
		free(handle);
		return NULL;
	}

	return handle;
}

/* Release the handle to libxc, free resources, etc. */
void xi_uninit(xi_handle *handle)
{
	close (handle->fd);
	free (handle);
}

/* Make simple xen version hypervisor calls */
static int xi_make_xen_version_hypercall(xi_handle *handle, long *vnum,
					 xen_extraversion_t *ver)
{
	privcmd_hypercall_t privcmd;
	multicall_entry_t multicall[2];
	int ret = 0;

	/* set up for doing hypercall */
	privcmd.op = __HYPERVISOR_multicall;
	privcmd.arg[0] = (unsigned long)multicall;
	privcmd.arg[1] = 2;

	/* first one to get xen version number */
	multicall[0].op = __HYPERVISOR_xen_version;
	multicall[0].args[0] = (unsigned long)XENVER_version;

	/* second to get xen version flag */
	multicall[1].op = __HYPERVISOR_xen_version;
	multicall[1].args[0] = (unsigned long)XENVER_extraversion;
	multicall[1].args[1] = (unsigned long)ver;

	if (mlock( &privcmd, sizeof(privcmd_hypercall_t)) < 0) {
		perror("Failed to mlock privcmd structure");
		return -1;
	}

	if (mlock( multicall, sizeof(multicall_entry_t)) < 0) {
		perror("Failed to mlock multicall_entry structure");
		munlock( &multicall, sizeof(multicall_entry_t));
		return -1;
	}

	if (ioctl( handle->fd, IOCTL_PRIVCMD_HYPERCALL, &privcmd) < 0) {
		perror("Hypercall failed");
		ret = -1;
	}

	*vnum = multicall[0].result;

	munlock( &privcmd, sizeof(privcmd_hypercall_t));
	munlock( &multicall, sizeof(multicall_entry_t));

	return ret;
}

/* Make Xen Dom0 op hypervisor call */
static int xi_make_dom0_op(xi_handle *handle, dom0_op_t *dom_op,
			   int dom_opcode)
{
	privcmd_hypercall_t privcmd;
	int ret = 0;

	/* set up for doing hypercall */
	privcmd.op = __HYPERVISOR_dom0_op;
	privcmd.arg[0] = (unsigned long)dom_op;
	dom_op->cmd = dom_opcode;
	dom_op->interface_version = DOM0_INTERFACE_VERSION;

	if (mlock( &privcmd, sizeof(privcmd_hypercall_t)) < 0) {
		perror("Failed to mlock privcmd structure");
		return -1;
	}

	if (mlock( dom_op, sizeof(dom0_op_t)) < 0) {
		perror("Failed to mlock dom0_op structure");
		munlock( &privcmd, sizeof(privcmd_hypercall_t));
		return -1;
	}

	if (ioctl( handle->fd, IOCTL_PRIVCMD_HYPERCALL, &privcmd) < 0) {
		perror("Hypercall failed");
		ret = -1;
	}

	munlock( &privcmd, sizeof(privcmd_hypercall_t));
	munlock( dom_op, sizeof(dom0_op_t));

	return ret;
}

/* Obtain domain data from dom0 */
int xi_get_physinfo(xi_handle *handle, dom0_physinfo_t *physinfo)
{
	dom0_op_t op;

	if (xi_make_dom0_op(handle, &op, DOM0_PHYSINFO) < 0) {
		perror("DOM0_PHYSINFO Hypercall failed");
		return -1;
	}

	*physinfo = op.u.physinfo;
	return 0;
}

/* Obtain domain data from dom0 */
int xi_get_domaininfolist(xi_handle *handle, dom0_getdomaininfo_t *info,
                          unsigned int first_domain, unsigned int max_domains)
{
	dom0_op_t op;
	op.u.getdomaininfolist.first_domain = first_domain;
	op.u.getdomaininfolist.max_domains = max_domains;
	op.u.getdomaininfolist.buffer = info;

	if (mlock( info, max_domains * sizeof(dom0_getdomaininfo_t)) < 0) {
		perror("Failed to mlock domaininfo array");
		return -1;
	}

	if (xi_make_dom0_op(handle, &op, DOM0_GETDOMAININFOLIST) < 0) {
		perror("DOM0_GETDOMAININFOLIST Hypercall failed");
		return -1;
	}

	return op.u.getdomaininfolist.num_domains;
}

/* Returns cpu usage data from dom0 */
long long xi_get_vcpu_usage(xi_handle *handle, unsigned int domain,
                            unsigned int vcpu)
{
	dom0_op_t op;
	op.u.getvcpucontext.domain = domain;
	op.u.getvcpucontext.vcpu = vcpu;
	op.u.getvcpucontext.ctxt = NULL;

	if (xi_make_dom0_op(handle, &op, DOM0_GETVCPUCONTEXT) < 0) {
		perror("DOM0_GETVCPUCONTEXT Hypercall failed");
		return -1;
	}

	return op.u.getvcpucontext.cpu_time;
}

/* gets xen version information from hypervisor */
int xi_get_xen_version(xi_handle *handle, long *vnum, xen_extraversion_t *ver)
{
	/* gets the XENVER_version and XENVER_extraversion */
	if (xi_make_xen_version_hypercall( handle, vnum, ver) < 0) {
		perror("XEN VERSION Hypercall failed");
		return -1;
	}

	return 0;
}
