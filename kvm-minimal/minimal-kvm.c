// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_MEM_SIZE	0x4000
#define GUEST_CODE_ADDR	0x1000
#define DEBUG_IO_PORT	0xe9

static const uint8_t guest_code[] = {
	0xb0, 'H', 0xe6, DEBUG_IO_PORT,
	0xb0, 'e', 0xe6, DEBUG_IO_PORT,
	0xb0, 'l', 0xe6, DEBUG_IO_PORT,
	0xb0, 'l', 0xe6, DEBUG_IO_PORT,
	0xb0, 'o', 0xe6, DEBUG_IO_PORT,
	0xb0, ',', 0xe6, DEBUG_IO_PORT,
	0xb0, ' ', 0xe6, DEBUG_IO_PORT,
	0xb0, 'K', 0xe6, DEBUG_IO_PORT,
	0xb0, 'V', 0xe6, DEBUG_IO_PORT,
	0xb0, 'M', 0xe6, DEBUG_IO_PORT,
	0xb0, '!', 0xe6, DEBUG_IO_PORT,
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xf4,
};

static void die(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

static int kvm_ioctl(int fd, unsigned long request, void *arg, const char *name)
{
	int ret;

	ret = ioctl(fd, request, arg);
	if (ret < 0) {
		perror(name);
		return ret;
	}

	return ret;
}

static int open_kvm(void)
{
	int fd, version;

	fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die("open /dev/kvm");

	version = kvm_ioctl(fd, KVM_GET_API_VERSION, NULL,
			    "KVM_GET_API_VERSION");
	if (version < 0)
		exit(EXIT_FAILURE);
	if (version != KVM_API_VERSION) {
		fprintf(stderr, "unexpected KVM API version: %d\n", version);
		exit(EXIT_FAILURE);
	}

	printf("KVM API version: %d\n", version);
	return fd;
}

static int setup_guest_memory(int vm_fd, void **guest_mem)
{
	struct kvm_userspace_memory_region region;
	void *mem;

	mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		perror("mmap guest memory");
		return -1;
	}

	memcpy((uint8_t *)mem + GUEST_CODE_ADDR, guest_code, sizeof(guest_code));

	region = (struct kvm_userspace_memory_region) {
		.slot = 0,
		.flags = 0,
		.guest_phys_addr = 0,
		.memory_size = GUEST_MEM_SIZE,
		.userspace_addr = (uintptr_t)mem,
	};

	if (kvm_ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region,
		      "KVM_SET_USER_MEMORY_REGION") < 0) {
		munmap(mem, GUEST_MEM_SIZE);
		return -1;
	}

	*guest_mem = mem;
	return 0;
}

static struct kvm_run *mmap_kvm_run(int kvm_fd, int vcpu_fd, int *mmap_size)
{
	struct kvm_run *run;

	*mmap_size = kvm_ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, NULL,
			       "KVM_GET_VCPU_MMAP_SIZE");
	if (*mmap_size < 0)
		return MAP_FAILED;

	run = mmap(NULL, *mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   vcpu_fd, 0);
	if (run == MAP_FAILED)
		perror("mmap kvm_run");

	return run;
}

static int setup_real_mode(int vcpu_fd)
{
	struct kvm_sregs sregs;
	struct kvm_regs regs;

	if (kvm_ioctl(vcpu_fd, KVM_GET_SREGS, &sregs, "KVM_GET_SREGS") < 0)
		return -1;

	sregs.cs.selector = 0;
	sregs.cs.base = 0;
	sregs.cs.limit = 0xffff;
	sregs.ds.selector = 0;
	sregs.ds.base = 0;
	sregs.ds.limit = 0xffff;
	sregs.es.selector = 0;
	sregs.es.base = 0;
	sregs.es.limit = 0xffff;
	sregs.fs.selector = 0;
	sregs.fs.base = 0;
	sregs.fs.limit = 0xffff;
	sregs.gs.selector = 0;
	sregs.gs.base = 0;
	sregs.gs.limit = 0xffff;
	sregs.ss.selector = 0;
	sregs.ss.base = 0;
	sregs.ss.limit = 0xffff;
	sregs.cr0 &= ~1ULL;

	if (kvm_ioctl(vcpu_fd, KVM_SET_SREGS, &sregs, "KVM_SET_SREGS") < 0)
		return -1;

	memset(&regs, 0, sizeof(regs));
	regs.rip = GUEST_CODE_ADDR;
	regs.rsp = 0x2000;
	regs.rbp = 0x2000;
	regs.rflags = 0x2;

	return kvm_ioctl(vcpu_fd, KVM_SET_REGS, &regs, "KVM_SET_REGS");
}

static void handle_io_exit(struct kvm_run *run, unsigned int *io_exits)
{
	uint8_t *data;
	uint32_t i;

	if (run->io.direction != KVM_EXIT_IO_OUT ||
	    run->io.port != DEBUG_IO_PORT || run->io.size != 1) {
		fprintf(stderr,
			"unexpected IO exit: direction=%u port=0x%x size=%u count=%u\n",
			run->io.direction, run->io.port, run->io.size,
			run->io.count);
		exit(EXIT_FAILURE);
	}

	data = (uint8_t *)run + run->io.data_offset;
	for (i = 0; i < run->io.count; i++)
		putchar(data[i]);
	fflush(stdout);
	(*io_exits)++;
}

static void run_vcpu(int vcpu_fd, struct kvm_run *run)
{
	unsigned int io_exits = 0;

	printf("guest output: ");
	fflush(stdout);

	for (;;) {
		if (ioctl(vcpu_fd, KVM_RUN, 0) < 0) {
			if (errno == EINTR)
				continue;
			die("KVM_RUN");
		}

		switch (run->exit_reason) {
		case KVM_EXIT_IO:
			handle_io_exit(run, &io_exits);
			break;
		case KVM_EXIT_HLT:
			printf("KVM_EXIT_HLT\n");
			printf("io exits: %u\n", io_exits);
			return;
		default:
			fprintf(stderr, "unexpected exit reason: %u\n",
				run->exit_reason);
			exit(EXIT_FAILURE);
		}
	}
}

int main(void)
{
	struct kvm_run *run = MAP_FAILED;
	void *guest_mem = MAP_FAILED;
	int kvm_fd = -1, vm_fd = -1, vcpu_fd = -1;
	int run_mmap_size = 0;
	int ret = EXIT_FAILURE;

	kvm_fd = open_kvm();

	vm_fd = kvm_ioctl(kvm_fd, KVM_CREATE_VM, NULL, "KVM_CREATE_VM");
	if (vm_fd < 0)
		goto out;

	if (setup_guest_memory(vm_fd, &guest_mem) < 0)
		goto out;

	vcpu_fd = kvm_ioctl(vm_fd, KVM_CREATE_VCPU, NULL, "KVM_CREATE_VCPU");
	if (vcpu_fd < 0)
		goto out;

	run = mmap_kvm_run(kvm_fd, vcpu_fd, &run_mmap_size);
	if (run == MAP_FAILED)
		goto out;

	if (setup_real_mode(vcpu_fd) < 0)
		goto out;

	run_vcpu(vcpu_fd, run);
	ret = EXIT_SUCCESS;

out:
	if (run != MAP_FAILED)
		munmap(run, run_mmap_size);
	if (guest_mem != MAP_FAILED)
		munmap(guest_mem, GUEST_MEM_SIZE);
	if (vcpu_fd >= 0)
		close(vcpu_fd);
	if (vm_fd >= 0)
		close(vm_fd);
	if (kvm_fd >= 0)
		close(kvm_fd);

	return ret;
}
