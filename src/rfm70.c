#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqreturn.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/cdev.h>
#include <linux/kfifo.h>
#include <linux/poll.h>
#include <linux/timekeeping.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include "rfm70.h"

#ifndef DRIVER_VERSION
#define DRIVER_VERSION "unknown"
#endif
#define DRIVER_NAME "rfm70"

/* RMF70 SPI commands. */
#define	C_R_REGISTER		0x00
#define	C_W_REGISTER		0x20
#define	C_R_RX_PAYLOAD		0x61
#define	C_W_TX_PAYLOAD		0xa0
#define	C_FLUSH_TX		0xe1
#define	C_FLUSH_RX		0xe2
#define	C_REUSE_TX_PL		0xe3
#define	C_ACTIVATE		0x50
#define	C_R_RX_PL_WID		0x60
#define	C_W_ACK_PAYLOAD		0xa8
#define	C_W_TX_PAYLOAD_NO_ACK	0xb0
#define	C_NOP			0xff

/* Sub commands for C_ACTIVATE. */
#define	ACTIVATE_TOGGLE		0x53
#define	ACTIVATE_FEATURES	0x73

/*
 * The RFM70 has two banks of registers, and the client must switch between
 * them as required. We conceal this detail from the user by setting
 * BANK_FLAG for register addresses in bank 1 and looking for this bit in
 * rfm70_command().
 */
#define	BANK_FLAG		0x80

#define	R_CONFIG		0x00
#define	B_CONFIG_MASK_RX_DR	0x40
#define	B_CONFIG_MASK_TX_DS	0x20
#define	B_CONFIG_MASK_MAX_RT	0x10
#define	B_CONFIG_EN_CRC		0x08
#define	B_CONFIG_CRCO		0x04
#define	B_CONFIG_PWR_UP		0x02
#define	B_CONFIG_PRIM_RX	0x01
#define	R_EN_AA			0x01
#define	R_EN_RXADDR		0x02
#define	R_SETUP_AW		0x03
#define	R_SETUP_RETR		0x04
#define	R_RF_CH			0x05
#define	R_RF_SETUP		0x06
#define	R_STATUS		0x07
#define	B_STATUS_RBANK		0x80
#define	B_STATUS_RX_DR		0x40
#define	B_STATUS_TX_DS		0x20
#define	B_STATUS_MAX_RT		0x10
#define	B_STATUS_TX_FULL	0x01
#define	M_STATUS_RX_P_NO	0x0e
#define	R_RX_ADDR_P0		0x0a
#define	R_RX_ADDR_P1		0x0b
#define	R_RX_ADDR_P2		0x0c
#define	R_RX_ADDR_P3		0x0d
#define	R_RX_ADDR_P4		0x0e
#define	R_RX_ADDR_P5		0x0f
#define R_TX_ADDR		0x10
#define	R_FIFO_STATUS		0x17
#define B_FIFO_STATUS_TX_REUSE	0x40
#define B_FIFO_STATUS_TX_FULL	0x20
#define B_FIFO_STATUS_TX_EMPTY	0x10
#define B_FIFO_STATUS_RX_FULL	0x02
#define B_FIFO_STATUS_RX_EMPTY	0x01
#define	R_DYNPD			0x1c
#define	R_FEATURE		0x1d
#define B_FEATURE_EN_DPL	0x04
#define	B_FEATURE_EN_ACK_PAY	0x02
#define	B_FEATURE_EN_DYN_ACK	0x01
#define	R_RB1_00		(BANK_FLAG | 0x00)
#define	R_RB1_01		(BANK_FLAG | 0x01)
#define	R_RB1_02		(BANK_FLAG | 0x02)
#define	R_RB1_03		(BANK_FLAG | 0x03)
#define	R_RB1_04		(BANK_FLAG | 0x04)
#define	R_RB1_05		(BANK_FLAG | 0x05)
#define	R_RB1_06		(BANK_FLAG | 0x06)
#define	R_RB1_07		(BANK_FLAG | 0x07)
#define	R_CHIP_ID		(BANK_FLAG | 0x08)
#define	R_CHIP_ID_RFM70_ID	0x63
#define	R_RB1_09		(BANK_FLAG | 0x09)
#define	R_RB1_0A		(BANK_FLAG | 0x0a)
#define	R_RB1_0B		(BANK_FLAG | 0x0b)
#define	R_RB1_0C		(BANK_FLAG | 0x0c)
#define	R_NEW_FEATURE		(BANK_FLAG | 0x0d)
#define	R_RAMP			(BANK_FLAG | 0x0e)

#define	RFM70_MODE_POWER_DOWN	0
#define	RFM70_MODE_STANDBY_ONE	1
#define	RFM70_MODE_RX		2

#define	RFM70_BUFFER_SIZE	1024
#define	RFM70_N_PIPES		6
#define	RFM70_MAX_CHANNEL	127

struct rfm70_dev {
	struct rfm70_variant *variant;
	int id;
	struct gpio_desc *ce;
	struct mutex spi_lock;	/* serialises rfm70_command() */
	struct completion write_completion;
	bool write_succeeded;
	int register_bank;
	struct spi_device *spi;
	struct cdev cdev;
	spinlock_t kflock;	/* to protect the kfifo */
	struct kfifo_rec_ptr_1 kfifo;
	struct device *device;
	dev_t dev;
	wait_queue_head_t read_wq;
};

struct rfm70_variant {
	char *name;
	struct ida ida;
	int (*init)(struct spi_device *spi, struct rfm70_dev *rfm70_dev);
};

static irqreturn_t ihandler(int, void *);

static struct class *rfm70_class;
static struct of_device_id rfm70_ids[];

/*
 * spi_lock should be held across this function to isolate file operations from
 * the interrupt handler.
 */
static int rfm70_command(struct spi_device *spi, u8 command, u8 address, void *data,
			 unsigned int len)
{
	u8 txbuf[64];
	int r;
	bool write;
	void *rxbuf;
	unsigned int n_rx;
	unsigned int n_tx;

	switch (command) {
	case C_R_REGISTER:
	case C_W_REGISTER:
	{
		struct rfm70_dev *rfm70_dev = spi_get_drvdata(spi);

		if (address != R_STATUS &&
		    rfm70_dev->register_bank ^ (address & BANK_FLAG)) {
			txbuf[0] = C_ACTIVATE;
			txbuf[1] = ACTIVATE_TOGGLE;
			r = spi_write_then_read(spi, txbuf, 2, NULL, 0);
			if (r < 0)
				return r;
			rfm70_dev->register_bank = 1 - rfm70_dev->register_bank;
		}

		write = (command == C_W_REGISTER);
		command |= (address & ~BANK_FLAG);
		break;
	}
	case C_W_ACK_PAYLOAD:
		write = true;
		command |= address;
		break;
	case C_W_TX_PAYLOAD:
	case C_ACTIVATE:
	case C_W_TX_PAYLOAD_NO_ACK:
		write = true;
		break;
	default:
		write = false;
		break;
	}

	txbuf[0] = command;
	if (write) {
		memcpy(txbuf + 1, data, len);
		n_tx = 1 + len;
		rxbuf = NULL;
		n_rx = 0;
	} else {
		n_tx = 1;
		rxbuf = data;
		n_rx = len;
	}

	r = spi_write_then_read(spi, txbuf, n_tx, rxbuf, n_rx);
	if (r < 0)
		return r;

	return 0;
}

static int rfm70_config_set_bits(struct spi_device *spi, u8 reg, u8 mask, u8 bits)
{
	int r;
	u8 val;

	if (bits & ~mask)
		return -EINVAL;
	r = rfm70_command(spi, C_R_REGISTER, reg, &val, 1);
	if (r < 0 || (val & mask) == bits)
		return r;
	val = (val & ~mask) | bits;
	return rfm70_command(spi, C_W_REGISTER, reg, &val, 1);
}

static int set_mode(struct spi_device *spi, int mode)
{
	int r = 0;
	struct rfm70_dev *rfm70_dev = spi_get_drvdata(spi);
	struct gpio_desc *ce = rfm70_dev->ce;

	switch (mode) {
	case RFM70_MODE_POWER_DOWN:
		gpiod_set_value(ce, 0);
		return rfm70_config_set_bits(spi, R_CONFIG, B_CONFIG_PWR_UP, 0);
	case RFM70_MODE_STANDBY_ONE:
		gpiod_set_value(ce, 0);
		return rfm70_config_set_bits(spi, R_CONFIG, B_CONFIG_PWR_UP | B_CONFIG_PRIM_RX, B_CONFIG_PWR_UP);
	case RFM70_MODE_RX:
		gpiod_set_value(ce, 0);
		r = rfm70_config_set_bits(spi, R_CONFIG, B_CONFIG_PWR_UP | B_CONFIG_PRIM_RX,
					  B_CONFIG_PWR_UP | B_CONFIG_PRIM_RX);
		if (r < 0)
			return r;
		gpiod_set_value(ce, 1);
		return 0;
	default:
		dev_err(&spi->dev, "invalid mode\n");
		return -EINVAL;
	}
}

static int nrf24l01_init(struct spi_device *spi, struct rfm70_dev *)
{
	dev_info(&spi->dev, "initialising nRF24L01");
	return set_mode(spi, RFM70_MODE_POWER_DOWN);
}

static int rfm70_init(struct spi_device *spi, struct rfm70_dev *rfm70_dev)
{
	size_t status;
	unsigned int chip_id;
	/*
	 * The datasheet calls for initialising various registers with specific
	 * values but gives no explanation.  Note that arrays are LSB first
	 * except for registers 0 through 8 in register bank 1 which are MSB
	 * first.
	 */
	struct magic {
		u8 address;
		u8 data[16];
		unsigned int len;
	} magic[] = {
		{R_RB1_00, {0x40, 0x4b, 0x01, 0xe2}, 4},
		{R_RB1_01, {0xc0, 0x4b, 0x00, 0x00}, 4},
		{R_RB1_02, {0xd0, 0xfc, 0x8c, 0x02}, 4},
		{R_RB1_03, {0x99, 0x00, 0x39, 0x41}, 4},
		{R_RB1_04, {0xf9, 0x9e, 0x86, 0x0b}, 4},
		{R_RB1_05, {0x24, 0x06, 0x7f, 0xa6}, 4},
		{R_RB1_06, {0x00, 0x00, 0x00, 0x00}, 4},
		{R_RB1_07, {0x00, 0x00, 0x00, 0x00}, 4},
		{R_CHIP_ID, {0x00, 0x00, 0x00, 0x00}, 4},
		{R_RB1_09, {0x00, 0x00, 0x00, 0x00}, 4},
		{R_RB1_0A, {0x00, 0x00, 0x00, 0x00}, 4},
		{R_RB1_0B, {0x00, 0x00, 0x00, 0x00}, 4},
		{R_RB1_0C, {0x00, 0x12, 0x73, 0x00}, 4},
		{R_NEW_FEATURE, {0x36, 0xb4, 0x80, 0x00}, 4},
		{R_RAMP, {0x41, 0x20, 0x08, 0x04, 0x81, 0x20, 0xcf, 0xf7, 0xfe,
		    0xff, 0xff}, 11},
		/*
		 * The manufacturer's sample code concludes by toggling bits 25
		 * and 26 in the most significant byte of register 4 in bank 1.
		 */
		{R_RB1_04, {0xff, 0x9e, 0x86, 0x0b}, 4},
		{R_RB1_04, {0xf9, 0x9e, 0x86, 0x0b}, 4},
	};
	int r;

	dev_info(&spi->dev, "initialising RFM70");

	status = spi_w8r8(spi, C_R_REGISTER | R_STATUS);
	if (status < 0)
		return status;
	rfm70_dev->register_bank = (status & B_STATUS_RBANK) != 0;

	if (rfm70_command(spi, C_R_REGISTER, R_CHIP_ID, &chip_id, 4) < 0) {
		dev_err(&spi->dev, "cannot read chip id\n");
		return -ENODEV;
	}
	if (chip_id != R_CHIP_ID_RFM70_ID) {
		dev_err(&spi->dev, "unknown chip id 0x%x\n", chip_id);
		return -ENODEV;
	}

	dev_info(&spi->dev, "HopeRF RFM70 detected\n");

	r = set_mode(spi, RFM70_MODE_POWER_DOWN);
	if (r < 0)
		return r;

	for (int i = 0; i < ARRAY_SIZE(magic); i++) {
		r = rfm70_command(spi, C_W_REGISTER, magic[i].address,
				  magic[i].data, magic[i].len);
		if (r < 0)
			return r;
	}

	return 0;
}

static int common_init(struct spi_device *spi, struct rfm70_dev *rfm70_dev)
{
	int feature;
	int r;
	u8 status;

	/*
	 * Some specific features must be enabled by setting appropriate bits in
	 * R_FEATURE but this register must itself be enabled.  The only way to
	 * know whether it is currently enabled is to test whether it can be set
	 * non-zero.
	 */
	feature = B_FEATURE_EN_DYN_ACK;
	r = rfm70_command(spi, C_W_REGISTER, R_FEATURE, &feature, 1);
	if (r < 0)
		return r;
	r = rfm70_command(spi, C_R_REGISTER, R_FEATURE, &feature, 1);
	if (r < 0)
		return r;

	if (feature == 0) {
		u8 activate = ACTIVATE_FEATURES;

		r = rfm70_command(spi, C_ACTIVATE, 0, &activate, 1);
	} else {
		feature = 0;
		r = rfm70_command(spi, C_W_REGISTER, R_FEATURE, &feature, 1);
	}
	if (r < 0)
		return r;

	r = rfm70_command(spi, C_R_REGISTER, R_STATUS, &status, 1);
	if (r < 0)
		return r;
	return rfm70_command(spi, C_W_REGISTER, R_STATUS, &status, 1);
}

static long rfm70_ioctl(struct file *file, unsigned int cmd, unsigned long argp)
{
	struct rfm70_dev *rfm70_dev = file->private_data;
	struct spi_device *spi = rfm70_dev->spi;

	switch (cmd) {
	case RFM70_IOC_SET_CONFIG:
	{
		struct rfm70_config rfm70_config;
		int r;
		u8 aw;
		u8 enable;
		u8 dpl;
		u8 aa;
		u8 crc;
		u8 config;
		u8 rf_setup;
		u8 retry;
		int mode;

		if (copy_from_user(&rfm70_config, (void __user *)argp, sizeof(rfm70_config)))
			return -EFAULT;

		mutex_lock(&rfm70_dev->spi_lock);

		r = set_mode(rfm70_dev->spi, RFM70_MODE_POWER_DOWN);
		if (r < 0)
			goto set_config_out;

		aw = rfm70_config.address_width;
		if (aw != 3 && aw != 4 && aw != 5) {
			r = -EINVAL;
			goto set_config_out;
		}
		aw -= 2;
		r = rfm70_command(spi, C_R_REGISTER, R_SETUP_AW, &aw, 1);
		if (r < 0)
			goto set_config_out;

		enable = 0;
		dpl = 0;
		aa = 0;
		for (int i = 0; i < RFM70_N_PIPES; i++) {
			struct pipe *p = &rfm70_config.pipes[i];

			if (p->enable)
				enable |= 1 << i;
			else
				continue;

			r = rfm70_command(rfm70_dev->spi, C_W_REGISTER, R_RX_ADDR_P0 + i, p->rx_address,
					  (i > 1) ? 1 : rfm70_config.address_width);
			if (r < 0)
				goto set_config_out;

			if (p->dpl)
				dpl |= 1 << i;

			if (p->aa)
				aa |= 1 << i;
		}

		r = rfm70_command(spi, C_W_REGISTER, R_EN_RXADDR, &enable, 1);
		if (r < 0)
			goto set_config_out;

		if (dpl) {
			u8 feature;

			r = rfm70_command(spi, C_R_REGISTER, R_FEATURE, &feature, 1);
			if (r < 0)
				goto set_config_out;
			feature |= B_FEATURE_EN_DPL;
			r = rfm70_command(spi, C_W_REGISTER, R_FEATURE, &feature, 1);
			if (r < 0)
				goto set_config_out;
		}
		r = rfm70_command(spi, C_W_REGISTER, R_DYNPD, &dpl, 1);
		if (r < 0)
			goto set_config_out;

		r = rfm70_command(spi, C_W_REGISTER, R_EN_AA, &aa, 1);
		if (r < 0)
			goto set_config_out;

		r = rfm70_command(rfm70_dev->spi, C_W_REGISTER, R_TX_ADDR, rfm70_config.tx_address,
				  rfm70_config.address_width);
		if (r < 0)
			goto set_config_out;

		if (rfm70_config.channel > RFM70_MAX_CHANNEL) {
			r = -EINVAL;
			goto set_config_out;
		}
		r = rfm70_command(rfm70_dev->spi, C_W_REGISTER, R_RF_CH, &rfm70_config.channel, 1);
		if (r < 0)
			goto set_config_out;

		switch (rfm70_config.crc) {
		case 0:
			crc = 0x00;
			break;
		case 1:
			crc = B_CONFIG_EN_CRC;
			break;
		case 2:
			crc = B_CONFIG_EN_CRC | B_CONFIG_CRCO;
			break;
		default:
			r = -EINVAL;
			goto set_config_out;
		}
		r = rfm70_command(spi, C_R_REGISTER, R_CONFIG, &config, 1);
		if (r < 0)
			goto set_config_out;
		config |= crc;
		r = rfm70_command(spi, C_W_REGISTER, R_CONFIG, &config, 1);
		if (r < 0)
			goto set_config_out;

		switch (rfm70_config.power) {
		case RFM70_PWR_m10DBM:
		case RFM70_PWR_m5DBM:
		case RFM70_PWR_0DBM:
		case RFM70_PWR_5DBM:
			break;
		default:
			r = -EINVAL;
			goto set_config_out;
		}

		switch (rfm70_config.dr) {
		case RFM70_DR_1MBPS:
		case RFM70_DR_2MBPS:
			break;
		default:
			r = -EINVAL;
			goto set_config_out;
		}

		switch (rfm70_config.lna) {
		case RFM70_LNA_LOW:
		case RFM70_LNA_HIGH:
			break;
		default:
			r = -EINVAL;
			goto set_config_out;
		}

		rf_setup = rfm70_config.power | rfm70_config.dr | rfm70_config.lna;
		r = rfm70_command(spi, C_W_REGISTER, R_RF_SETUP, &rf_setup, 1);
		if (r < 0)
			goto set_config_out;

		switch (rfm70_config.ard) {
		case RFM70_RETRY_ARD_250:
		case RFM70_RETRY_ARD_500:
		case RFM70_RETRY_ARD_750:
		case RFM70_RETRY_ARD_1000:
		case RFM70_RETRY_ARD_1250:
		case RFM70_RETRY_ARD_1500:
		case RFM70_RETRY_ARD_1750:
		case RFM70_RETRY_ARD_2000:
		case RFM70_RETRY_ARD_2250:
		case RFM70_RETRY_ARD_2500:
		case RFM70_RETRY_ARD_2750:
		case RFM70_RETRY_ARD_3000:
		case RFM70_RETRY_ARD_3250:
		case RFM70_RETRY_ARD_3500:
		case RFM70_RETRY_ARD_3750:
		case RFM70_RETRY_ARD_4000:
			break;
		default:
			r = -EINVAL;
			goto set_config_out;
		}

		if (rfm70_config.arc > 15) {
			r = -EINVAL;
			goto set_config_out;
		}

		retry = rfm70_config.ard | rfm70_config.arc;
		r = rfm70_command(spi, C_W_REGISTER, R_SETUP_RETR, &retry, 1);
		if (r < 0)
			goto set_config_out;

		r = rfm70_command(rfm70_dev->spi, C_FLUSH_RX, 0, NULL, 0);
		if (r < 0)
			goto set_config_out;
		r = rfm70_command(rfm70_dev->spi, C_FLUSH_TX, 0, NULL, 0);
		if (r < 0)
			goto set_config_out;

		mode = (file->f_mode & FMODE_READ) ? RFM70_MODE_RX : RFM70_MODE_STANDBY_ONE;
		r = set_mode(rfm70_dev->spi, mode);

set_config_out:
		mutex_unlock(&rfm70_dev->spi_lock);

		return r;
	}

	default:
		return -EINVAL;
	}
}

static irqreturn_t ihandler(int irq, void *data)
{
	struct spi_device *spi = data;
	struct rfm70_dev *rfm70_dev = spi_get_drvdata(spi);
	u8 status;
	int r = IRQ_NONE;
	u8 irqs;
	u8 fifo_status;

	mutex_lock(&rfm70_dev->spi_lock);

	if (rfm70_command(spi, C_R_REGISTER, R_STATUS, &status, 1) < 0)
		goto out;
	irqs = status & (B_STATUS_RX_DR | B_STATUS_TX_DS | B_STATUS_MAX_RT);
	if (irqs == 0)
		goto out;

	r = IRQ_HANDLED;

	if (irqs & B_STATUS_RX_DR) {
		while (true) {
			u64 ns;
			u8 buffer[RFM70_MAX_RECORD_LEN];
			int rxlen;
			size_t recsize;

			if (rfm70_command(spi, C_R_REGISTER, R_FIFO_STATUS, &fifo_status, 1) < 0)
				break;

			if (fifo_status & B_FIFO_STATUS_RX_EMPTY)
				break;

			if (rfm70_command(spi, C_R_REGISTER, R_STATUS, &status, 1) < 0)
				break;

			ns = ktime_get_real_ns();
			memcpy(buffer, &ns, sizeof(ns));

			buffer[sizeof(ns)] = (status & M_STATUS_RX_P_NO) >> 1;

			if (rfm70_command(spi, C_R_RX_PL_WID, 0, &rxlen, 1) < 0)
				break;
			if (rxlen > RFM70_MAX_PACKET_LEN) {
				dev_info(&spi->dev, "dropping packed with claimed length %d\n", rxlen);
				break;
			}

			if (rfm70_command(spi, C_R_RX_PAYLOAD, 0, buffer + sizeof(ns) + 1, rxlen) < 0)
				break;

			recsize = sizeof(u64) + 1 + rxlen;

			spin_lock(&rfm70_dev->kflock);
			while (kfifo_avail(&rfm70_dev->kfifo) < recsize)
				kfifo_skip(&rfm70_dev->kfifo);
			if (kfifo_in(&rfm70_dev->kfifo, buffer, recsize) != recsize) {
				dev_err(&spi->dev, "unable to store incoming record");
				goto out;
			}
			spin_unlock(&rfm70_dev->kflock);

			wake_up_interruptible(&rfm70_dev->read_wq);
		}
	}

	if (irqs & (B_STATUS_TX_DS | B_STATUS_MAX_RT)) {
		switch (irqs & (B_STATUS_TX_DS | B_STATUS_MAX_RT)) {
		case B_STATUS_TX_DS:
			rfm70_dev->write_succeeded = true;
			break;
		case B_STATUS_MAX_RT:
			rfm70_dev->write_succeeded = false;
			break;
		case B_STATUS_TX_DS | B_STATUS_MAX_RT:
			/* Such a combination should never occur. */
			rfm70_dev->write_succeeded =
				!rfm70_command(spi, C_R_REGISTER, R_FIFO_STATUS, &fifo_status, 1) &&
				(fifo_status & B_FIFO_STATUS_TX_EMPTY);
			break;
		}
		if (!rfm70_dev->write_succeeded)
			(void)rfm70_command(spi, C_FLUSH_TX, 0, NULL, 0);
		complete(&rfm70_dev->write_completion);
	}
out:
	(void)rfm70_command(spi, C_W_REGISTER, R_STATUS, &irqs, 1);
	mutex_unlock(&rfm70_dev->spi_lock);
	return r;
}

static unsigned int rfm70_poll(struct file *file, poll_table *wait)
{
	struct rfm70_dev *rfm70_dev = file->private_data;
	bool is_empty;

	poll_wait(file, &rfm70_dev->read_wq, wait);
	spin_lock(&rfm70_dev->kflock);
	is_empty = kfifo_is_empty(&rfm70_dev->kfifo);
	spin_unlock(&rfm70_dev->kflock);
	return is_empty ? 0 : (POLLIN | POLLRDNORM);
}

static ssize_t rfm70_write(struct file *file, const char __user *ubuf, size_t count, loff_t *offset)
{
	u8 kbuf[RFM70_MAX_PACKET_LEN];
	size_t packet_len;
	unsigned long not_copied;
	int r;
	struct rfm70_dev *rfm70_dev = file->private_data;
	struct spi_device *spi = rfm70_dev->spi;

	packet_len = (count < RFM70_MAX_PACKET_LEN) ? count : RFM70_MAX_PACKET_LEN;
	not_copied = copy_from_user(kbuf, ubuf, packet_len);
	packet_len -= not_copied;
	if (packet_len == 0)
		return 0;

	reinit_completion(&rfm70_dev->write_completion);

	mutex_lock(&rfm70_dev->spi_lock);

	/* If we are write-only then we are already in STANDBY-I. */
	if (file->f_mode & FMODE_READ) {
		r = set_mode(spi, RFM70_MODE_STANDBY_ONE);
		if (r < 0)
			goto out;
	}

	/* Upload the payload. */
	r = rfm70_command(spi, C_W_TX_PAYLOAD, 0, kbuf, packet_len);
	if (r < 0)
		goto out;

	/* A short pulse on CE takes us to TX and thence back to STANDBY-I. */
	gpiod_set_value(rfm70_dev->ce, 1);
	msleep(1);
	gpiod_set_value(rfm70_dev->ce, 0);

	if (file->f_mode & FMODE_READ)
		r = set_mode(spi, RFM70_MODE_RX);
out:
	mutex_unlock(&rfm70_dev->spi_lock);
	if (r < 0)
		return r;

	r = wait_for_completion_interruptible(&rfm70_dev->write_completion);
	if (r)
		return r;

	return rfm70_dev->write_succeeded ? packet_len : -EIO;
}

static ssize_t rfm70_read(struct file *file, char __user *ubuf, size_t ubufsize, loff_t *offset)
{
	struct rfm70_dev *rfm70_dev = file->private_data;
	size_t reclen;
	size_t copied;
	u8 kbuf[RFM70_MAX_RECORD_LEN];

	while (kfifo_is_empty(&rfm70_dev->kfifo)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(rfm70_dev->read_wq, !kfifo_is_empty(&rfm70_dev->kfifo)))
			return -ERESTARTSYS;
	}

	spin_lock(&rfm70_dev->kflock);
	reclen = kfifo_peek_len(&rfm70_dev->kfifo);
	if (reclen > ubufsize) {
		spin_unlock(&rfm70_dev->kflock);
		return -EINVAL;
	}
	copied = kfifo_out(&rfm70_dev->kfifo, kbuf, reclen);
	spin_unlock(&rfm70_dev->kflock);

	return copy_to_user(ubuf, kbuf, reclen) ? -EFAULT : reclen;
}

static int rfm70_open(struct inode *inode, struct file *file)
{
	struct rfm70_dev *rfm70_dev = container_of(inode->i_cdev, struct rfm70_dev, cdev);
	struct spi_device *spi = rfm70_dev->spi;
	int r;

	file->private_data = rfm70_dev;

	r = request_threaded_irq(spi->irq, NULL, ihandler, IRQF_ONESHOT, DRIVER_NAME, spi);
	if (r < 0) {
		dev_err(&spi->dev, "request_threaded_irq() failed for irq %d: %d\n", spi->irq, r);
		return r;
	}

	return 0;
}

static int rfm70_release(struct inode *, struct file *file)
{
	struct rfm70_dev *rfm70_dev = file->private_data;
	struct spi_device *spi = rfm70_dev->spi;

	free_irq(spi->irq, spi);
	return 0;
}

struct file_operations rfm70_fops = {
	.owner =	THIS_MODULE,
	.read =		rfm70_read,
	.write =	rfm70_write,
	.open =		rfm70_open,
	.poll =		rfm70_poll,
	.unlocked_ioctl = rfm70_ioctl,
	.release =	rfm70_release,
};

static int rfm70_probe(struct spi_device *spi)
{
	struct rfm70_dev *rfm70_dev;
	const struct of_device_id *of_id;
	const struct spi_device_id *spi_id;
	int r;

	rfm70_dev = devm_kzalloc(&spi->dev, sizeof(*rfm70_dev), GFP_KERNEL);
	if (!rfm70_dev)
		goto err_devm_zalloc;

	spi_set_drvdata(spi, rfm70_dev);
	rfm70_dev->spi = spi;

	of_id = of_match_device(rfm70_ids, &spi->dev);
	if (of_id) {
		rfm70_dev->variant = (struct rfm70_variant *)of_id->data;
	} else {
		spi_id = spi_get_device_id(spi);
		if (spi_id) {
			rfm70_dev->variant = (struct rfm70_variant *)spi_id->driver_data;
		} else {
			dev_err(&spi->dev, "no matching device found\n");
			goto err_of_match_device;
		}
	}

	r = alloc_chrdev_region(&rfm70_dev->dev, 0, 1, DRIVER_NAME);
	if (r < 0) {
		dev_err(&spi->dev, "alloc_chrdev_region() failed\n");
		goto err_alloc_chrdev_region;
	}

	rfm70_dev->ce = devm_gpiod_get(&spi->dev, "ce", GPIOD_OUT_LOW);
	if (IS_ERR(rfm70_dev->ce)) {
		dev_err(&spi->dev, "Failed to get chip enable GPIO\n");
		r = PTR_ERR(rfm70_dev->ce);
		goto err_devm_gpiod_get;
	}

	rfm70_dev->id = ida_alloc(&rfm70_dev->variant->ida, GFP_KERNEL);
	if (rfm70_dev->id < 0) {
		dev_err(&spi->dev, "ida_alloc() failed\n");
		goto err_ida_alloc;
	}

	rfm70_dev->device = device_create(rfm70_class, &spi->dev, rfm70_dev->dev, NULL, "%s:%d",
					  rfm70_dev->variant->name, rfm70_dev->id);
	if (IS_ERR(rfm70_dev->device)) {
		dev_err(&spi->dev, "device_create() failed\n");
		r = PTR_ERR(rfm70_dev->device);
		goto err_device_create;
	}

	r = kfifo_alloc(&rfm70_dev->kfifo, RFM70_BUFFER_SIZE, GFP_KERNEL);
	if (r < 0) {
		dev_err(&spi->dev, "kfifo_alloc() failed\n");
		goto err_kfifo_alloc;
	}

	init_waitqueue_head(&rfm70_dev->read_wq);
	spin_lock_init(&rfm70_dev->kflock);
	mutex_init(&rfm70_dev->spi_lock);
	init_completion(&rfm70_dev->write_completion);

	r = rfm70_dev->variant->init(spi, rfm70_dev);
	if (r < 0) {
		dev_err(&spi->dev, "device-specific initialisation failed");
		goto err_variant_init;
	}
	r = common_init(spi, rfm70_dev);
	if (r < 0) {
		dev_err(&spi->dev, "device-common initialisation failed");
		goto err_common_init;
	}

	cdev_init(&rfm70_dev->cdev, &rfm70_fops);
	r = cdev_add(&rfm70_dev->cdev, rfm70_dev->dev, 1);
	if (r < 0) {
		dev_err(&spi->dev, "cdev_add() failed\n");
		goto err_cdev_init;
	}

	return 0;

err_cdev_init:
err_common_init:
err_variant_init:
	kfifo_free(&rfm70_dev->kfifo);
err_kfifo_alloc:
	device_destroy(rfm70_class, rfm70_dev->dev);
err_device_create:
	ida_free(&rfm70_dev->variant->ida, rfm70_dev->id);
err_ida_alloc:
err_devm_gpiod_get:
	unregister_chrdev_region(rfm70_dev->dev, 1);
err_alloc_chrdev_region:
err_of_match_device:
err_devm_zalloc:
	return r;
}

static void rfm70_remove(struct spi_device *spi)
{
	struct rfm70_dev *rfm70_dev = spi_get_drvdata(spi);

	cdev_del(&rfm70_dev->cdev);
	kfifo_free(&rfm70_dev->kfifo);
	device_destroy(rfm70_class, rfm70_dev->dev);
	ida_free(&rfm70_dev->variant->ida, rfm70_dev->id);
	unregister_chrdev_region(rfm70_dev->dev, 1);
}

struct rfm70_variant rfm70_variants[] = {
	{ .name = "rfm70", .init = &rfm70_init },
	{ .name = "nRF24L01", .init = &nrf24l01_init }
};

static struct of_device_id rfm70_ids[] = {
	{ .compatible = "hope,rfm70", .data = &rfm70_variants[0] },
	{ .compatible = "nordic,nRF24L01", .data = &rfm70_variants[1] },
	{},
};
MODULE_DEVICE_TABLE(of, rfm70_ids);

static struct spi_device_id rfm70[] = {
	{ "rfm70", (kernel_ulong_t)&rfm70_variants[0] },
	{ "nRF24L01", (kernel_ulong_t)&rfm70_variants[1] },
	{},
};
MODULE_DEVICE_TABLE(spi, rfm70);

static struct spi_driver rfm70_driver = {
	.probe = rfm70_probe,
	.remove = rfm70_remove,
	.id_table = rfm70,
	.driver = {
		.name = "rfm70",
		.of_match_table = rfm70_ids,
	},
};

static int __init rfm70_register(struct spi_driver *sdrv)
{
	rfm70_class = class_create("rfm70");
	if (IS_ERR(rfm70_class))
		return PTR_ERR(rfm70_class);
	for (int i = 0; i < ARRAY_SIZE(rfm70_variants); i++)
		ida_init(&rfm70_variants[i].ida);
	pr_info("%s: registering SPI driver (version %s)\n", DRIVER_NAME, DRIVER_VERSION);
	return spi_register_driver(sdrv);
}

static void __exit rfm70_unregister(struct spi_driver *sdrv)
{
	pr_info("%s: unregistering SPI driver (version %s)\n", DRIVER_NAME, DRIVER_VERSION);
	spi_unregister_driver(sdrv);
	for (int i = 0; i < ARRAY_SIZE(rfm70_variants); i++)
		ida_destroy(&rfm70_variants[i].ida);
	class_destroy(rfm70_class);
}

module_driver(rfm70_driver, rfm70_register, rfm70_unregister);

MODULE_AUTHOR("Robert Harris <robertmalcolmharris@googlemail.com>");
MODULE_DESCRIPTION("RFM70 radio transceiver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
