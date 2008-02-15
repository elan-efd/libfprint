/*
 * Shared functions between libfprint Authentec drivers
 * Copyright (C) 2007-2008 Daniel Drake <dsd@gentoo.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "aeslib"

#include <errno.h>

#include <libusb.h>
#include <glib.h>

#include "fp_internal.h"
#include "aeslib.h"

#define MAX_REGWRITES_PER_REQUEST	16

#define BULK_TIMEOUT	4000
#define EP_IN			(1 | LIBUSB_ENDPOINT_IN)
#define EP_OUT			(2 | LIBUSB_ENDPOINT_OUT)

struct write_regv_data {
	struct fp_img_dev *imgdev;
	unsigned int num_regs;
	const struct aes_regwrite *regs;
	unsigned int offset;
	aes_write_regv_cb callback;
	void *user_data;
};

static void continue_write_regv(struct write_regv_data *wdata);

/* libusb bulk callback for regv write completion transfer. continues the
 * transaction */
static void write_regv_trf_complete(libusb_dev_handle *devh,
	libusb_urb_handle *urbh, enum libusb_urb_cb_status status,
	unsigned char endpoint,	int rqlength, unsigned char *data,
	int actual_length, void *user_data)
{
	struct write_regv_data *wdata = user_data;

	g_free(data);
	libusb_urb_handle_free(urbh);

	if (status != FP_URB_COMPLETED)
		wdata->callback(wdata->imgdev, -EIO, wdata->user_data);
	else if (rqlength != actual_length)
		wdata->callback(wdata->imgdev, -EPROTO, wdata->user_data);
	else
		continue_write_regv(wdata);
}

/* write from wdata->offset to upper_bound (inclusive) of wdata->regs */
static int do_write_regv(struct write_regv_data *wdata, int upper_bound)
{
	unsigned int offset = wdata->offset;
	unsigned int num = upper_bound - offset + 1;
	size_t alloc_size = num * 2;
	unsigned char *data = g_malloc(alloc_size);
	unsigned int i;
	size_t data_offset = 0;
	struct libusb_urb_handle *urbh;
	struct libusb_bulk_transfer msg = {
		.endpoint = EP_OUT,
		.data = data,
		.length = alloc_size,
	};

	for (i = offset; i < offset + num; i++) {
		const struct aes_regwrite *regwrite = &wdata->regs[i];
		data[data_offset++] = regwrite->reg;
		data[data_offset++] = regwrite->value;
	}

	urbh = libusb_async_bulk_transfer(wdata->imgdev->udev, &msg,
		write_regv_trf_complete, wdata, BULK_TIMEOUT);
	if (!urbh) {
		g_free(data);
		return -EIO;
	}

	return 0;
}

/* write the next batch of registers to be written, or if there are no more,
 * indicate completion to the caller */
static void continue_write_regv(struct write_regv_data *wdata)
{
	unsigned int offset = wdata->offset;
	unsigned int regs_remaining;
	unsigned int limit;
	unsigned int upper_bound;
	int i;
	int r;

	/* skip all zeros and ensure there is still work to do */
	while (TRUE) {
		if (offset >= wdata->num_regs) {
			fp_dbg("all registers written");
			wdata->callback(wdata->imgdev, 0, wdata->user_data);
			return;
		}
		if (wdata->regs[offset].reg)
			break;
		offset++;
	}

	wdata->offset = offset;
	regs_remaining = wdata->num_regs - offset;
	limit = MIN(regs_remaining, MAX_REGWRITES_PER_REQUEST);
	upper_bound = offset + limit - 1;

	/* determine if we can write the entire of the regs at once, or if there
	 * is a zero dividing things up */
	for (i = offset; i <= upper_bound; i++)
		if (!wdata->regs[i].reg) {
			upper_bound = i - 1;
			break;
		}

	r = do_write_regv(wdata, upper_bound);
	if (r < 0) {
		wdata->callback(wdata->imgdev, r, wdata->user_data);
		return;
	}

	wdata->offset = upper_bound + 1;
}

/* write a load of registers to the device, combining multiple writes in a
 * single URB up to a limit. insert writes to non-existent register 0 to force
 * specific groups of writes to be separated by different URBs. */
void aes_write_regv(struct fp_img_dev *dev, const struct aes_regwrite *regs,
	unsigned int num_regs, aes_write_regv_cb callback, void *user_data)
{
	struct write_regv_data *wdata = g_malloc(sizeof(*wdata));
	fp_dbg("write %d regs", num_regs);
	wdata->imgdev = dev;
	wdata->num_regs = num_regs;
	wdata->regs = regs;
	wdata->offset = 0;
	wdata->callback = callback;
	wdata->user_data = user_data;
	continue_write_regv(wdata);
}

void aes_assemble_image(unsigned char *input, size_t width, size_t height,
	unsigned char *output)
{
	size_t row, column;

	for (column = 0; column < width; column++) {
		for (row = 0; row < height; row += 2) {
			output[width * row + column] = (*input & 0x07) * 36;
			output[width * (row + 1) + column] = ((*input & 0x70) >> 4) * 36;
			input++;
		}
	}
}

