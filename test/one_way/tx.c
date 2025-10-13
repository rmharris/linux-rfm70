#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "rfm70.h"


int
main(int argc, char **argv)
{
	int fd;
	struct rfm70_config config = {
		.address_width = 5,
		.pipes = {
			[0] = {
				.rx_address = { 0x9a, 0x78, 0x56, 0x34, 0x12 },
				.enable = 1,
				.dpl = 1,
				.aa = 1
			}
		},
		.tx_address = { 0x9a, 0x78, 0x56, 0x34, 0x12 },
		.channel = 42,
		.crc = 2,
		.power = RFM70_PWR_m10DBM,
		.dr = RFM70_DR_1MBPS,
		.lna = RFM70_LNA_LOW,
		.ard = RFM70_RETRY_ARD_250,
		.arc = RFM70_RETRY_ARC_1
	};
	char *buf = NULL;
	size_t bufsize;
	ssize_t n_read;
	ssize_t n_written;
	useconds_t usec;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <device>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if ((fd = open(argv[1], O_RDWR)) < 0) {
		fprintf(stderr, "open(%s): %s\n", argv[1], strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (ioctl(fd, RFM70_IOC_SET_CONFIG, &config) < 0) {
		perror("ioctl:");
		return (-1);
	}

	while ((n_read  = getline(&buf, &bufsize, stdin)) > 0) {		
		ssize_t n_written;
		uint8_t *cursor = buf;
		size_t n_remaining = n_read;

		while ((n_written = write(fd, cursor, n_remaining)) > 0) {
			cursor += n_written;
			n_remaining -= n_written;
		}

		if (n_written < 0) {
			perror("write");
			return (EXIT_FAILURE);
		}
	}

	if (n_read < 0) {
		perror("getline");
		exit(1);
	}

	return (EXIT_SUCCESS);
}
