/*
 * Copyright (c) 2016, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Intel Corporation nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *         Keyon Jie <yang.jie@linux.intel.com>
 */

#include <sof/debug.h>
#include <sof/timer.h>
#include <sof/interrupt.h>
#include <sof/ipc.h>
#include <sof/mailbox.h>
#include <sof/sof.h>
#include <sof/stream.h>
#include <sof/dai.h>
#include <sof/dma.h>
#include <sof/alloc.h>
#include <sof/wait.h>
#include <sof/trace.h>
#include <sof/ssp.h>
#include <platform/interrupt.h>
#include <platform/mailbox.h>
#include <platform/shim.h>
#include <platform/dma.h>
#include <platform/platform.h>
#include <sof/audio/component.h>
#include <sof/audio/pipeline.h>
#include <uapi/ipc.h>
#include <sof/intel-ipc.h>

extern struct ipc *_ipc;

/* test code to check working IRQ */
static void irq_handler(void *arg)
{
	uint32_t dipct;
	uint32_t dipcie;
	uint32_t msg = 0;

	tracev_ipc("IRQ");

	dipct = ipc_read(IPC_DIPCT);
	dipcie = ipc_read(IPC_DIPCIE);

	/* new message from host */
	if (dipct & IPC_DIPCT_BUSY) {
		trace_ipc("Nms");

		/* mask Busy interrupt */
		ipc_write(IPC_DIPCCTL, ipc_read(IPC_DIPCCTL) & ~IPC_DIPCCTL_IPCTBIE);

		msg = dipct & IPC_DIPCT_MSG_MASK;

		/* TODO: place message in Q and process later */
		/* It's not Q ATM, may overwrite */
		if (_ipc->host_pending)
			trace_ipc_error("Pen");
		_ipc->host_msg = msg;
		_ipc->host_pending = 1;

	}

	/* reply message(done) from host */
	if (dipcie & IPC_DIPCIE_DONE) {
		tracev_ipc("Rpy");
		/* mask Done interrupt */
		ipc_write(IPC_DIPCCTL, ipc_read(IPC_DIPCCTL) & ~IPC_DIPCCTL_IPCIDIE);

		/* clear DONE bit - tell host we have completed the operation */
		ipc_write(IPC_DIPCIE, ipc_read(IPC_DIPCIE) |IPC_DIPCIE_DONE);

		/* unmask Done interrupt */
		ipc_write(IPC_DIPCCTL, ipc_read(IPC_DIPCCTL) | IPC_DIPCCTL_IPCIDIE);
	}

}

void ipc_platform_do_cmd(struct ipc *ipc)
{
	struct intel_ipc_data *iipc = ipc_get_drvdata(ipc);
	struct sof_ipc_reply reply;
	int32_t err;

	trace_ipc("Cmd");

	/* perform command and return any error */
	err = ipc_cmd();
	if (err > 0) {
		goto done; /* reply created and copied by cmd() */
	} else if (err < 0) {
		/* send std error reply */
		reply.error = err;
	} else if (err == 0) {
		/* send std reply */
		reply.error = 0;
	}

	/* send std error/ok reply */
	reply.hdr.cmd = SOF_IPC_GLB_REPLY;
	reply.hdr.size = sizeof(reply);
	mailbox_hostbox_write(0, &reply, sizeof(reply));

done:
	ipc->host_pending = 0;

	/* write 1 to clear busy, and trigger interrupt to host*/
	ipc_write(IPC_DIPCT, ipc_read(IPC_DIPCT) |IPC_DIPCT_BUSY );

	/* unmask Busy interrupt */
	ipc_write(IPC_DIPCCTL, ipc_read(IPC_DIPCCTL) | IPC_DIPCCTL_IPCTBIE);

	// TODO: signal audio work to enter D3 in normal context
	/* are we about to enter D3 ? */
	if (iipc->pm_prepare_D3) {
		while (1)
			wait_for_interrupt(0);
	}

	tracev_ipc("CmD");

}

void ipc_platform_send_msg(struct ipc *ipc)
{
	struct ipc_msg *msg;
	uint32_t flags;

	spin_lock_irq(&ipc->lock, flags);

	/* any messages to send ? */
	if (list_is_empty(&ipc->msg_list)) {
		ipc->dsp_pending = 0;
		goto out;
	}

	/* now send the message */
	msg = list_first_item(&ipc->msg_list, struct ipc_msg, list);
	mailbox_dspbox_write(0, msg->tx_data, msg->tx_size);
	list_item_del(&msg->list);
	ipc->dsp_msg = msg;
	tracev_ipc("Msg");

	/* now interrupt host to tell it we have message sent */
	ipc_write(IPC_DIPCIE, 0);
	ipc_write(IPC_DIPCI, 0x80000000 | msg->header);

	list_item_append(&msg->list, &ipc->empty_list);

out:
	spin_unlock_irq(&ipc->lock, flags);
}

int platform_ipc_init(struct ipc *ipc)
{
	struct intel_ipc_data *iipc;
	int i;

	_ipc = ipc;

	/* init ipc data */
	iipc = rzalloc(RZONE_SYS, SOF_MEM_CAPS_RAM,
		sizeof(struct intel_ipc_data));
	ipc_set_drvdata(_ipc, iipc);
	_ipc->dsp_msg = NULL;
	list_init(&ipc->empty_list);
	list_init(&ipc->msg_list);
	spinlock_init(&ipc->lock);
	for (i = 0; i < MSG_QUEUE_SIZE; i++)
		list_item_prepend(&ipc->message[i].list, &ipc->empty_list);

	/* allocate page table buffer */
	iipc->page_table = rballoc(RZONE_SYS, SOF_MEM_CAPS_RAM,
			HOST_PAGE_SIZE);
	if (iipc->page_table)
		bzero(iipc->page_table, HOST_PAGE_SIZE);

	/* dma */
	iipc->dmac = dma_get(DMA_GP_LP_DMAC0);

	/* PM */
	iipc->pm_prepare_D3 = 0;

	/* configure interrupt */
	interrupt_register(PLATFORM_IPC_INTERUPT, irq_handler, NULL);
	interrupt_enable(PLATFORM_IPC_INTERUPT);

	/* enable IPC interrupts from host */
	ipc_write(IPC_DIPCCTL, IPC_DIPCCTL_IPCIDIE | IPC_DIPCCTL_IPCTBIE);

	return 0;
}
