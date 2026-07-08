/*
 * Sample application to benchmark latency, jitter and WCET of execution on the application domain
 * (Linux on Cortex-A53 cores) vs the real-time domain (baremetal Cortex-R5 cores).
 * This source file contains the host application implementation.
 *
 * Flow:
 *   1. Connect to the R5 "rpmsg-openamp-bench" channel
 *   2. Run the same bench_fun() kernel locally on this A53 core, timing each of
 *      SAMPLES_NUMBER iterations with CLOCK_MONOTONIC_RAW (nanoseconds).
 *   3. Send STARTBENCH_MSG to the R5; it runs the kernel SAMPLES_NUMBER times timing
 *      each with the R5 cycle counter (PMCCNTR) and streams the samples back in
 *      batches; collect them here.
 *   4. Write both datasets to CSV for offline analysis
 *
 *
 * For a clean A53 measurement, run pinned + real-time, e.g.:
 *   sudo chrt -f 99 taskset -c 3 ./linux_bench -p 3
 * and, for the "disturbed" configuration, load the sibling cores with stress-ng.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <sched.h>
#include <sys/select.h>
#include <linux/rpmsg.h>

#include "../common/common.h"
#include "bench_common.h"

#define PR_DBG(fmt, args ...) printf("%s():%u "fmt, __func__, __LINE__, ##args)
#define RPMSG_BUS_SYS "/sys/bus/rpmsg"

#define WARMUP_ITERS 1000
#define DEFAULT_CPU  3
#define RECV_TIMEOUT_SEC 5

/* Compiler barrier: prevents the compiler from hoisting/CSE-ing bench_fun() out of
 * the timing loop. On the R5 bench_fun is noinline (can't be CSE'd); on the A53 it
 * may be inlined, so we need this to guarantee it actually runs every iteration. */
#define barrier() asm volatile("" ::: "memory")

/*-----------------------------------------------------------------------------*
 *  A53 local benchmark (mirror of the R5 benchmark(), timed with clock_gettime)
 *-----------------------------------------------------------------------------*/
static void a53_benchmark(sample_ele_type *samples)
{
	struct timespec t0, t1;
	volatile uint32_t acc = 0;

	/* same input data as the R5 side */
	for (int i = 0; i < VECTOR_LENGTH; i++)
		bench_vector[i] = (vector_ele_type)rand();

	/* warm the caches / branch predictor, discard */
	for (int i = 0; i < WARMUP_ITERS; i++) {
		barrier();
		acc = bench_fun();
	}

	for (int i = 0; i < SAMPLES_NUMBER; i++) {
		barrier();
		clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
		acc = bench_fun();
		clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
		barrier();

		uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL
			    + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
		/* saturate rather than wrap on an absurd (>4.29 s) stall */
		samples[i] = (ns > UINT32_MAX) ? UINT32_MAX : (sample_ele_type)ns;
	}
	(void)acc;
}

/*-----------------------------------------------------------------------------*
 *  Helpers
 *-----------------------------------------------------------------------------*/
static void pin_to_cpu(int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) < 0)
		perror("sched_setaffinity");
	else
		printf("pinned to CPU %d\n", cpu);
}

static int dump_samples(const char *path, const sample_ele_type *s, size_t n,
			const char *unit)
{
	FILE *f = fopen(path, "w");
	if (!f) {
		perror(path);
		return -1;
	}
	fprintf(f, "# unit=%s count=%zu\n", unit, n);
	for (size_t i = 0; i < n; i++)
		fprintf(f, "%u\n", (unsigned)s[i]);
	fclose(f);
	printf("wrote %zu samples (%s) to %s\n", n, unit, path);
	return 0;
}

static int send_cmd(int fd, uint32_t cmd)
{
	if (write(fd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		perror("write cmd");
		return -1;
	}
	return 0;
}

/* Collect SAMPLES_NUMBER u32 samples streamed back by the R5 (batched messages). */
static int collect_r5_samples(int fd, sample_ele_type *samples)
{
	size_t got = 0;
	uint32_t buf[256];	/* one R5 message is <= 100 samples (400 B) */

	while (got < SAMPLES_NUMBER) {
		fd_set rfds;
		struct timeval tv = { .tv_sec = RECV_TIMEOUT_SEC, .tv_usec = 0 };

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);

		int sret = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (sret < 0) {
			if (errno == EINTR)
				continue;
			perror("select");
			return -1;
		}
		if (sret == 0) {
			fprintf(stderr, "timeout: got %zu/%d samples from R5\n",
				got, SAMPLES_NUMBER);
			return -1;
		}

		ssize_t nb = read(fd, buf, sizeof(buf));
		if (nb < 0) {
			if (errno == EAGAIN)
				continue;
			perror("read");
			return -1;
		}
		if (nb == 0)
			continue;

		size_t ns = (size_t)nb / sizeof(uint32_t);
		for (size_t k = 0; k < ns && got < SAMPLES_NUMBER; k++)
			samples[got++] = buf[k];
	}
	return 0;
}

/*-----------------------------------------------------------------------------*
 *  main
 *-----------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	int ret, opt, charfd, fd;
	int cpu = DEFAULT_CPU, do_shutdown = 0;
	char rpmsg_dev[NAME_MAX]           = "virtio0." RPMSG_SERVICE_NAME ".-1.0";
	char rpmsg_ctrl_dev_name[NAME_MAX] = "virtio0.rpmsg_ctrl.0.0";
	char rpmsg_char_name[16];
	char fpath[2 * NAME_MAX];
	char out_a53[NAME_MAX] = "a53_samples.csv";
	char out_r5[NAME_MAX]  = "r5_samples.csv";
	char ept_dev_name[16];
	char ept_dev_path[32];
	struct rpmsg_endpoint_info eptinfo = {
		.name = RPMSG_SERVICE_NAME, .src = 0, .dst = 0
	};
	/* static: 2 x 40 KB arrays belong in .bss, not on the stack */
	static sample_ele_type a53_samples[SAMPLES_NUMBER];
	static sample_ele_type r5_samples[SAMPLES_NUMBER];

	printf("\r\n bench host start \r\n");

	/* auto-discover the channel device by name (may be overridden by -d) */
	lookup_channel(rpmsg_dev, &eptinfo);

	while ((opt = getopt(argc, argv, "d:c:o:r:p:kh")) != -1) {
		switch (opt) {
		case 'd':
			memset(rpmsg_dev, 0, sizeof(rpmsg_dev));
			strncpy(rpmsg_dev, optarg, sizeof(rpmsg_dev) - 1);
			break;
		case 'c':
			memset(rpmsg_ctrl_dev_name, 0, sizeof(rpmsg_ctrl_dev_name));
			strncpy(rpmsg_ctrl_dev_name, optarg, sizeof(rpmsg_ctrl_dev_name) - 1);
			break;
		case 'o':
			strncpy(out_a53, optarg, sizeof(out_a53) - 1);
			break;
		case 'r':
			strncpy(out_r5, optarg, sizeof(out_r5) - 1);
			break;
		case 'p':
			cpu = atoi(optarg);
			break;
		case 'k':
			do_shutdown = 1;
			break;
		default:
			printf("usage: %s [-d dev] [-c ctrl] [-o a53.csv] [-r r5.csv] [-p cpu] [-k]\n",
			       argv[0]);
			printf("  -d rpmsg device name     (default %s)\n", rpmsg_dev);
			printf("  -c rpmsg control device  (default virtio0.rpmsg_ctrl.0.0)\n");
			printf("  -o A53 output CSV (ns)   (default a53_samples.csv)\n");
			printf("  -r R5 output CSV (cycles)(default r5_samples.csv)\n");
			printf("  -p CPU to pin to         (default %d)\n", DEFAULT_CPU);
			printf("  -k send SHUTDOWN to the R5 when done\n");
			return -EINVAL;
		}
	}

	/* pin so the A53 run stays on one, stable core */
	pin_to_cpu(cpu);

	/* --- connect to the R5 bench channel (same flow as echo_test.c) --- */
	sprintf(fpath, RPMSG_BUS_SYS "/devices/%s", rpmsg_dev);
	if (access(fpath, F_OK)) {
		fprintf(stderr, "access(%s): %s\n", fpath, strerror(errno));
		fprintf(stderr, "is the R5 bench firmware running and the channel up?\n");
		return -EINVAL;
	}
	ret = bind_rpmsg_chrdev(rpmsg_dev);
	if (ret < 0)
		return ret;

	/* kernel >= 6.0 has a new path for the rpmsg_ctrl device; fall back for < 6.0 */
	charfd = get_rpmsg_chrdev_fd(rpmsg_ctrl_dev_name, rpmsg_char_name);
	if (charfd < 0) {
		charfd = get_rpmsg_chrdev_fd(rpmsg_dev, rpmsg_char_name);
		if (charfd < 0)
			return charfd;
	}

	PR_DBG("create_ept: %s[src=%#x,dst=%#x]\n",
	       eptinfo.name, eptinfo.src, eptinfo.dst);
	ret = app_rpmsg_create_ept(charfd, &eptinfo);
	if (ret) {
		fprintf(stderr, "app_rpmsg_create_ept: %s\n", strerror(errno));
		return -EINVAL;
	}
	if (!get_rpmsg_ept_dev_name(rpmsg_char_name, eptinfo.name, ept_dev_name))
		return -EINVAL;
	sprintf(ept_dev_path, "/dev/%s", ept_dev_name);

	printf("open %s\n", ept_dev_path);
	fd = open(ept_dev_path, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror(ept_dev_path);
		close(charfd);
		return -1;
	}

	/* --- 1) A53 local benchmark --- */
	printf("running A53 benchmark (%d samples)...\n", SAMPLES_NUMBER);
	a53_benchmark(a53_samples);
	dump_samples(out_a53, a53_samples, SAMPLES_NUMBER, "ns");

	/* --- 2) trigger the R5 benchmark and collect its samples --- */
	printf("triggering R5 benchmark...\n");
	if (send_cmd(fd, STARTBENCH_MSG) < 0)
		goto out;
	if (collect_r5_samples(fd, r5_samples) < 0) {
		fprintf(stderr, "R5 sample collection failed\n");
		goto out;
	}
	dump_samples(out_r5, r5_samples, SAMPLES_NUMBER, "cycles");

	printf("done. A53 -> %s (ns), R5 -> %s (cycles)\n", out_a53, out_r5);

out:
	if (do_shutdown)
		send_cmd(fd, SHUTDOWN_MSG);
	close(fd);
	if (charfd >= 0)
		close(charfd);
	return 0;
}
