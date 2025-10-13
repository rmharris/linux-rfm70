#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/uio.h>
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
		.lna = RFM70_LNA_LOW
	};
	ssize_t n;
	uint8_t buffer[RFM70_MAX_RECORD_LEN];
	size_t msglen;
	
	if (argc != 3) {
		fprintf(stderr, "usage: %s <device> <message>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if ((fd = open(argv[1], O_RDWR)) < 0) {
		fprintf(stderr, "open(%s): %s\n", argv[1], strerror(errno));
		exit(EXIT_FAILURE);
	}

	msglen = strlen(argv[2]);

	if (ioctl(fd, RFM70_IOC_SET_CONFIG, &config) < 0) {
		perror("ioctl:");
		exit(EXIT_FAILURE);
	}

	for (;;) {
		n = write(fd, argv[2], msglen);
		fprintf(stdout, "-> %*s (%s)\n", msglen, argv[2], n == msglen ? "ok" : "failed");

		if ((n = read(fd, buffer, sizeof(buffer))) < 0) {
			perror("read():");
			exit(EXIT_FAILURE);
		}
		fprintf(stdout, "<- %*s\n", n - RFM70_OFFSET_PACKET, buffer + RFM70_OFFSET_PACKET);

		sleep(2);
	}
}
