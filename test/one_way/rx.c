#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
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
				.enable = true,
				.dpl = true,
				.aa = true
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
	
	if (argc != 2) {
		fprintf(stderr, "usage: %s <device>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if ((fd = open(argv[1], O_RDONLY)) < 0) {
		fprintf(stderr, "open(%s): %s\n", argv[1], strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (ioctl(fd, RFM70_IOC_SET_CONFIG, &config) < 0) {
		perror("ioctl:");
		return (-1);
	}

	while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
		uint64_t ns;
		uint64_t ms;
		time_t s;
		struct tm *tm;
		uint8_t pipe;
		uint8_t *packet;
		char timestring[128];

		memcpy(&ns, buffer + RFM70_OFFSET_TIME, sizeof(uint64_t));
		ms = ns / 1000000;
		s = ms / 1000;
		tm = localtime(&s);
		strftime(timestring, sizeof(timestring), "%F %T", tm);

		pipe = buffer[RFM70_OFFSET_PIPE];

		packet = buffer + RFM70_OFFSET_PACKET;

		fprintf(stdout, "%s.%03d pipe %d: ", timestring, ms - (s * 1000), pipe);
		for (int i = 0; i < n - 9; i++) {
			char c = packet[i];

			if (isprint(c))
				fprintf(stdout, "%c", c);
			else if (c == '\n')
				fprintf(stdout, "\\n");
			else
				fprintf(stdout, ".");
		}
		fprintf(stdout, "\n");
	}

	if (n < 0) {
		perror("read():");
		exit(1);
	}

	return (EXIT_SUCCESS);
}
