/*
 * Sample application to benchmark latency, jitter and WCET of execution on the application domain
 * (Linux on Cortex-A53 cores) vs the real-time domain (baremetal Cortex-R5 cores).
 * This source file contains the remote application implementation.
 */

#include <stdio.h>
#include <openamp/open_amp.h>
#include <openamp/version.h>
#include <metal/alloc.h>
#include <metal/version.h>
#include "platform_info_common.h"
#include "xil_exception.h"

#include "bench_common.h"

#define LPRINTF(format, ...) metal_info(format, ##__VA_ARGS__)
#define LPERROR(format, ...) metal_err(format, ##__VA_ARGS__)

typedef enum {
  NO_REQ,
  SHUTDOWN_REQ,
  STARTBENCH_REQ
} request_t;

static struct rpmsg_endpoint lept;
static request_t req = NO_REQ;
static uint32_t host_addr = RPMSG_ADDR_ANY;	/* address of the host endpoint to reply to */


static TCM_TEXT void r5_benchmark(uint32_t *samples) {
  volatile uint32_t acc;
  uint32_t start_t, exec_t;

  for (int i=0; i<VECTOR_LENGTH; i++)
    bench_vector[i] = (vector_ele_type) rand();
  
  for (int i=0; i<SAMPLES_NUMBER; i++)
    samples[i] = -1;

  Xil_ExceptionDisable();
  for (int i=0; i<SAMPLES_NUMBER; i++) {
    start_t = ccnt();
    acc = bench_fun();
    exec_t = ccnt()-start_t;
    samples[i] = exec_t;
  }
  Xil_ExceptionEnable();
}


/*-----------------------------------------------------------------------------*
 *  RPMSG endpoint callbacks
 *-----------------------------------------------------------------------------*/
static int rpmsg_endpoint_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
			     uint32_t src, void *priv)
{
	(void)priv;

  if (len >= sizeof(uint32_t)) {
    uint32_t cmd = (*(uint32_t*)data);
    /* On reception of a shutdown we signal the application to terminate */
    if (cmd == SHUTDOWN_MSG) {
      LPRINTF("shutdown message is received.\r\n");
      req = SHUTDOWN_REQ;
      return RPMSG_SUCCESS;
    }

    /* On reception of a start bench signal we start the benchmark and report the results */
    if (cmd == STARTBENCH_MSG) {
      LPRINTF("start benchmark message is received.\r\n");
      host_addr = src;          /* remember who asked so we can reply there */
      req = STARTBENCH_REQ;
      return RPMSG_SUCCESS;
    }
  }

	/* Send data back to host */
	if (rpmsg_send(ept, data, len) < 0) {
		LPERROR("rpmsg_send failed\r\n");
	}
	return RPMSG_SUCCESS;
}



static void rpmsg_service_unbind(struct rpmsg_endpoint *ept)
{
	(void)ept;
	LPRINTF("unexpected Remote endpoint destroy\r\n");
	req = SHUTDOWN_REQ;
}

/*-----------------------------------------------------------------------------*
 *  Application
 *-----------------------------------------------------------------------------*/
int bench_app(struct rpmsg_device *rdev, void *priv)
{
	int ret;
  static sample_ele_type samples[SAMPLES_NUMBER];

  pmu_enable();

	/* Initialize RPMSG framework */
	metal_dbg("Try to create rpmsg endpoint.\r\n");

	ret = rpmsg_create_ept(&lept, rdev, RPMSG_SERVICE_NAME,
			       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
			       rpmsg_endpoint_cb,
			       rpmsg_service_unbind);
	if (ret) {
		LPERROR("Failed to create endpoint.\r\n");
		return -1;
	}

	metal_log(METAL_LOG_NOTICE,
		  "created rpmsg channel %s, src=0x%x, dst=0x%x\r\n",
		   RPMSG_SERVICE_NAME, lept.addr, lept.dest_addr);

	metal_dbg("RPMsg device TX buffer size: %#x\r\n", rpmsg_get_tx_buffer_size(&lept));
	metal_dbg("RPMsg device RX buffer size: %#x\r\n", rpmsg_get_rx_buffer_size(&lept));

	while(1) {
		platform_poll(priv);
		/* we got a shutdown request, exit */
		if (req == SHUTDOWN_REQ) {
			break;
		}
    else if (req == STARTBENCH_REQ) {
      r5_benchmark(samples);
      
      const size_t batch = 100;
      for (int i=0; i<SAMPLES_NUMBER; i+=batch) {
        size_t n = (SAMPLES_NUMBER - i < batch) ? (SAMPLES_NUMBER - i) : batch;
        if (rpmsg_sendto(&lept, (void*)&(samples[i]), n*sizeof(sample_ele_type), host_addr) < 0)
          LPERROR("rpmsg_send failed\r\n");
      }
      req = NO_REQ;
    }
	}
	rpmsg_destroy_ept(&lept);

	return 0;
}

/*-----------------------------------------------------------------------------*
 *  Application entry point
 *-----------------------------------------------------------------------------*/
int __attribute__((weak)) main(int argc, char *argv[])
{
	void *platform;
	struct rpmsg_device *rpdev;
	int ret;

	/* Initialize platform */
	ret = platform_init(argc, argv, &platform);
	if (ret) {
		LPERROR("Failed to initialize platform.\r\n");
		return ret;
	}

	LPRINTF("openamp lib version: %s (", openamp_version());
	LPRINTF("Major: %d, ", openamp_version_major());
	LPRINTF("Minor: %d, ", openamp_version_minor());
	LPRINTF("Patch: %d)\r\n", openamp_version_patch());

	LPRINTF("libmetal lib version: %s (", metal_ver());
	LPRINTF("Major: %d, ", metal_ver_major());
	LPRINTF("Minor: %d, ", metal_ver_minor());
	LPRINTF("Patch: %d)\r\n", metal_ver_patch());

	LPRINTF("Starting application...\r\n");

	rpdev = platform_create_rpmsg_vdev(platform, 0,
					   VIRTIO_DEV_DEVICE,
					   NULL, NULL);
	if (!rpdev) {
		LPERROR("Failed to create rpmsg virtio device.\r\n");
		ret = -1;
	} else {
		bench_app(rpdev, platform);
		platform_release_rpmsg_vdev(rpdev, platform);
		ret = 0;
	}

	LPRINTF("Stopping application...\r\n");
	platform_cleanup(platform);

	return ret;
}
