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
#define MMIO_TEST_ADDR	0x5000
#define IRQ_VECTOR	0x20
#define IRQ_HANDLER_ADDR 0x1200
#define BP_INT_VECTOR	0x03
#define BP_HANDLER_ADDR 0x1300
#define UD_INT_VECTOR	0x06
#define UD_HANDLER_ADDR 0x1400

static const uint8_t guest_code[] = {
	0x31, 0xc0,			/* xor eax, eax */
	0x0f, 0xa2,			/* cpuid */
	0x88, 0xd8, 0xe6, DEBUG_IO_PORT,	/* mov al, bl; out 0xe9, al */
	0x88, 0xf8, 0xe6, DEBUG_IO_PORT,	/* mov al, bh; out 0xe9, al */
	0x66, 0xc1, 0xeb, 0x10,		/* shr ebx, 16 */
	0x88, 0xd8, 0xe6, DEBUG_IO_PORT,
	0x88, 0xf8, 0xe6, DEBUG_IO_PORT,
	0x88, 0xd0, 0xe6, DEBUG_IO_PORT,	/* mov al, dl; out 0xe9, al */
	0x88, 0xf0, 0xe6, DEBUG_IO_PORT,	/* mov al, dh; out 0xe9, al */
	0x66, 0xc1, 0xea, 0x10,		/* shr edx, 16 */
	0x88, 0xd0, 0xe6, DEBUG_IO_PORT,
	0x88, 0xf0, 0xe6, DEBUG_IO_PORT,
	0x88, 0xc8, 0xe6, DEBUG_IO_PORT,	/* mov al, cl; out 0xe9, al */
	0x88, 0xe8, 0xe6, DEBUG_IO_PORT,	/* mov al, ch; out 0xe9, al */
	0x66, 0xc1, 0xe9, 0x10,		/* shr ecx, 16 */
	0x88, 0xc8, 0xe6, DEBUG_IO_PORT,
	0x88, 0xe8, 0xe6, DEBUG_IO_PORT,
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xc7, 0x06, 0x00, 0x50, 0x78, 0x56,	/* mov word [0x5000], 0x5678 */
	0xa1, 0x00, 0x50,			/* mov ax, [0x5000] */
	0xe6, DEBUG_IO_PORT,			/* out 0xe9, al */
	0x88, 0xe0, 0xe6, DEBUG_IO_PORT,	/* mov al, ah; out 0xe9, al */
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xcc,					/* int3 */
	0x0f, 0x0b,				/* ud2 */
	0xfb,					/* sti */
	0x90,					/* nop */
	0xf4,					/* hlt */
};

static const uint8_t irq_handler[] = {
	0xb0, 'I', 0xe6, DEBUG_IO_PORT,	/* mov al, 'I'; out 0xe9, al */
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xcf,					/* iret */
};

static const uint8_t bp_handler[] = {
	0xb0, 'B', 0xe6, DEBUG_IO_PORT,	/* mov al, 'B'; out 0xe9, al */
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xcf,					/* iret */
};

static const uint8_t ud_handler[] = {
	0x55,					/* push bp */
	0x89, 0xe5,				/* mov bp, sp */
	0x83, 0x46, 0x02, 0x02,		/* add word [bp + 2], 2 */
	0x5d,					/* pop bp */
	0xb0, 'U', 0xe6, DEBUG_IO_PORT,	/* mov al, 'U'; out 0xe9, al */
	0xb0, '\n', 0xe6, DEBUG_IO_PORT,
	0xcf,					/* iret */
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
	uint8_t *ivt_entry;
	void *mem;

	mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		perror("mmap guest memory");
		return -1;
	}

	memcpy((uint8_t *)mem + GUEST_CODE_ADDR, guest_code, sizeof(guest_code));
	memcpy((uint8_t *)mem + IRQ_HANDLER_ADDR, irq_handler, sizeof(irq_handler));
	memcpy((uint8_t *)mem + BP_HANDLER_ADDR, bp_handler, sizeof(bp_handler));
	memcpy((uint8_t *)mem + UD_HANDLER_ADDR, ud_handler, sizeof(ud_handler));

	ivt_entry = (uint8_t *)mem + IRQ_VECTOR * 4;
	ivt_entry[0] = IRQ_HANDLER_ADDR & 0xff;
	ivt_entry[1] = IRQ_HANDLER_ADDR >> 8;
	ivt_entry[2] = 0;
	ivt_entry[3] = 0;

	ivt_entry = (uint8_t *)mem + BP_INT_VECTOR * 4;
	ivt_entry[0] = BP_HANDLER_ADDR & 0xff;
	ivt_entry[1] = BP_HANDLER_ADDR >> 8;
	ivt_entry[2] = 0;
	ivt_entry[3] = 0;

	ivt_entry = (uint8_t *)mem + UD_INT_VECTOR * 4;
	ivt_entry[0] = UD_HANDLER_ADDR & 0xff;
	ivt_entry[1] = UD_HANDLER_ADDR >> 8;
	ivt_entry[2] = 0;
	ivt_entry[3] = 0;

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

static uint32_t pack4(char a, char b, char c, char d)
{
	return (uint32_t)a | (uint32_t)b << 8 |
	       (uint32_t)c << 16 | (uint32_t)d << 24;
}

static int setup_cpuid(int kvm_fd, int vcpu_fd)
{
	struct kvm_cpuid_entry2 *entry;
	struct kvm_cpuid2 *cpuid;
	int i, ret;

	cpuid = calloc(1, sizeof(*cpuid) + 100 * sizeof(cpuid->entries[0]));
	if (!cpuid) {
		perror("calloc cpuid");
		return -1;
	}
	cpuid->nent = 100;

	ret = kvm_ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid,
			"KVM_GET_SUPPORTED_CPUID");
	if (ret < 0)
		goto out;

	for (i = 0; i < (int)cpuid->nent; i++) {
		entry = &cpuid->entries[i];
		if (entry->function != 0 || entry->index != 0)
			continue;

		entry->ebx = pack4('K', 'V', 'M', 'D');
		entry->edx = pack4('E', 'M', 'O', '1');
		entry->ecx = pack4('2', '3', '4', '5');
		break;
	}

	if (i == (int)cpuid->nent) {
		fprintf(stderr, "missing CPUID leaf 0\n");
		ret = -1;
		goto out;
	}

	ret = kvm_ioctl(vcpu_fd, KVM_SET_CPUID2, cpuid, "KVM_SET_CPUID2");

out:
	free(cpuid);
	return ret;
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
	sregs.idt.base = 0;
	sregs.idt.limit = 0xffff;
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

static int inject_interrupt(int vcpu_fd)
{
	struct kvm_interrupt irq = {
		.irq = IRQ_VECTOR,
	};

	return kvm_ioctl(vcpu_fd, KVM_INTERRUPT, &irq, "KVM_INTERRUPT");
}

static void handle_mmio_exit(struct kvm_run *run, unsigned int *mmio_exits)
{
	uint32_t i;

	if (run->mmio.phys_addr != MMIO_TEST_ADDR || run->mmio.len != 2) {
		fprintf(stderr,
			"unexpected MMIO exit: addr=0x%llx len=%u is_write=%u\n",
			(unsigned long long)run->mmio.phys_addr, run->mmio.len,
			run->mmio.is_write);
		exit(EXIT_FAILURE);
	}

	if (run->mmio.is_write &&
	    (run->mmio.data[0] != 0x78 || run->mmio.data[1] != 0x56)) {
		fprintf(stderr, "unexpected MMIO write data\n");
		exit(EXIT_FAILURE);
	}

	if (!run->mmio.is_write) {
		run->mmio.data[0] = 'O';
		run->mmio.data[1] = 'K';
	}

	printf("KVM_EXIT_MMIO: addr=0x%llx len=%u is_write=%u data=0x",
	       (unsigned long long)run->mmio.phys_addr, run->mmio.len,
	       run->mmio.is_write);
	for (i = 0; i < run->mmio.len; i++)
		printf("%02x", run->mmio.data[i]);
	printf("\n");
	(*mmio_exits)++;
}

static void run_vcpu(int vcpu_fd, struct kvm_run *run)
{
	unsigned int io_exits = 0, mmio_exits = 0;
	int irq_injected = 0;

	printf("guest output: ");
	fflush(stdout);
	run->request_interrupt_window = 1;

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
		case KVM_EXIT_MMIO:
			handle_mmio_exit(run, &mmio_exits);
			break;
		case KVM_EXIT_IRQ_WINDOW_OPEN:
			printf("KVM_EXIT_IRQ_WINDOW_OPEN: inject IRQ 0x%x\n",
			       IRQ_VECTOR);
			run->request_interrupt_window = 0;
			if (inject_interrupt(vcpu_fd) < 0)
				exit(EXIT_FAILURE);
			irq_injected = 1;
			break;
		case KVM_EXIT_HLT:
			if (!irq_injected) {
				fprintf(stderr, "KVM_EXIT_HLT before IRQ window\n");
				exit(EXIT_FAILURE);
			}
			printf("KVM_EXIT_HLT\n");
			printf("io exits: %u\n", io_exits);
			printf("mmio exits: %u\n", mmio_exits);
			return;
		case KVM_EXIT_FAIL_ENTRY:
			fprintf(stderr, "KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason=0x%llx\n",
				(unsigned long long)run->fail_entry.hardware_entry_failure_reason);
			exit(EXIT_FAILURE);
		case KVM_EXIT_INTERNAL_ERROR:
			fprintf(stderr, "KVM_EXIT_INTERNAL_ERROR: suberror=0x%x\n",
				run->internal.suberror);
			exit(EXIT_FAILURE);
		case KVM_EXIT_SHUTDOWN:
			fprintf(stderr, "KVM_EXIT_SHUTDOWN\n");
			exit(EXIT_FAILURE);
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

	if (setup_cpuid(kvm_fd, vcpu_fd) < 0)
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
