#ifndef RFM70_H
#define RFM70_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct rfm70_config {
	/* The size of an address.  Must be 3, 4 or 5. */
	__u8 address_width;
	struct pipe {
		/*
		 * The receiving address of this pipe in LSB order.  For
		 * pipes 0 and 1 only the first address_width bytes are
		 * used.  For other pipes only the first byte is used.
		 */
		__u8	rx_address[5];
		/* Enable this pipe? */
		bool	enable;
		/* Enable Auto Acknowledgement for this pipe? */
		bool	aa;
		/* Enable Dynamic Payload Length for this pipe? */
		bool	dpl;
	} pipes[6];
	/* The address to which transmissions are sent. */
	__u8 tx_address[5];
	/*
	 * The frequency centre is given by (2400 + channel) MHz.  channel
	 * must be in the range 0--127, inclusive.
	 */
	__u8 channel;
	/*
	 * Number of bytes for Cyclic Redundancy Check;  must be one of
	 * 0 (no CRC), 1 or 2.
	 */
	__u8 crc;
	/* Transmission power.	Must be one of RFM70_PWR_, below. */
	__u8 power;
	/* Air Data Rate.  Must be one of RFM70_DR_, below. */
	__u8 dr;
	/* Low Noise Amplifier.  Must be one of RFM70_LNA_, below. */
	__u8 lna;
	/*
	 * Automatic Retransmission Delay.  Applicable only if the device can
	 * transmit.  Must be one of the RFM70_RETRY_ARD_ values below.
	 */
	__u8 ard;
	/*
	 * Automatic Retransmission Count.  Applicable only if the device can
	 * transmit.  Must be one of the RFM70_RETRY_ARC_ values below.
	 */
	__u8 arc;
};

#define RFM70_IOC_MAGIC 'R'

#define RFM70_IOC_SET_CONFIG		_IOW(RFM70_IOC_MAGIC, 1, struct rfm70_config)

#define	RFM70_DR_1MBPS			0x00
#define	RFM70_DR_2MBPS			0x08

#define	RFM70_PWR_m10DBM		0x00
#define	RFM70_PWR_m5DBM			0x02
#define	RFM70_PWR_0DBM			0x04
#define	RFM70_PWR_5DBM			0x06

#define	RFM70_LNA_LOW			0x00
#define	RFM70_LNA_HIGH			0x01

#define	RFM70_RETRY_ARD_250		0x00
#define	RFM70_RETRY_ARD_500		0x10
#define	RFM70_RETRY_ARD_750		0x20
#define	RFM70_RETRY_ARD_1000		0x30
#define	RFM70_RETRY_ARD_1250		0x40
#define	RFM70_RETRY_ARD_1500		0x50
#define	RFM70_RETRY_ARD_1750		0x60
#define	RFM70_RETRY_ARD_2000		0x70
#define	RFM70_RETRY_ARD_2250		0x80
#define	RFM70_RETRY_ARD_2500		0x90
#define	RFM70_RETRY_ARD_2750		0xa0
#define	RFM70_RETRY_ARD_3000		0xb0
#define	RFM70_RETRY_ARD_3250		0xc0
#define	RFM70_RETRY_ARD_3500		0xd0
#define	RFM70_RETRY_ARD_3750		0xe0
#define	RFM70_RETRY_ARD_4000		0xf0

#define	RFM70_RETRY_ARC_DIS		0x00
#define	RFM70_RETRY_ARC_1		0x01
#define	RFM70_RETRY_ARC_2		0x02
#define	RFM70_RETRY_ARC_3		0x03
#define	RFM70_RETRY_ARC_4		0x04
#define	RFM70_RETRY_ARC_5		0x05
#define	RFM70_RETRY_ARC_6		0x06
#define	RFM70_RETRY_ARC_7		0x07
#define	RFM70_RETRY_ARC_8		0x08
#define	RFM70_RETRY_ARC_9		0x09
#define	RFM70_RETRY_ARC_10		0x0a
#define	RFM70_RETRY_ARC_11		0x0b
#define	RFM70_RETRY_ARC_12		0x0c
#define	RFM70_RETRY_ARC_13		0x0d
#define	RFM70_RETRY_ARC_14		0x0e
#define	RFM70_RETRY_ARC_15		0x0f

#define	RFM70_MAX_PACKET_LEN		32
#define	RFM70_MAX_RECORD_LEN		(sizeof(__u64) + 1 + RFM70_MAX_PACKET_LEN)

#define	RFM70_OFFSET_TIME		0
#define	RFM70_OFFSET_PIPE		sizeof(__u64)
#define	RFM70_OFFSET_PACKET		(RFM70_OFFSET_PIPE + sizeof(__u8))

#endif	/* RFM70_H */
