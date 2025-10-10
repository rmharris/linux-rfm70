This is an out-of-tree Linux kernel driver for the HopeRF RFM70 radio
transceiver.  It also supports the nRF24L01, of which the former is a clone.

The driver was developed on a Raspberry Pi Zero 2 W and requires a Device
Tree overlay to describe the physical devices.  This package can build an
overlay for up to two devices.  For example,
```
cd dt
make
```
will create an overlay for a single RFM70.  This can be installed temporarily
with
```
dtoverlay overlay-rfm70.dtbo
```
or, to make this permanent,
```
cp overlay-rfm70.dtbo /boot/firmware/overlays
vi /boot/firmware/config.txt
```
add the line
```
dtoverlay=overlay-rfm70.dtbo
```
to the bottom, then reboot.

The device variant can be configured with CS0 and CS1.  For example, to build
an overlay describing an nRF24L01 on chip select 0 and an RFM70 on chip select 1
use
```
make CS0=nRF24L01 CS1=rfm70
```
For temporary use, the driver itself can be made and installed with
```
cd src
make
insmod rfm70.ko
```
A permanent installation requires git and dkms:
```
scripts/gen-dkms-conf.sh
dkms add .
dkms build linux-rfm70/v1.0.0
dkms install linux-rfm70/v1.0.0
modprobe rfm70
```
One installed, the driver will create a single character device for each
transceiver, e.g. `/dev/rfm70:0` for the first overlay, above, or
`/dev/nRF24L01:0` and `/dev/rfm70:0` for the second.  Each device supports a
single concurrent use;  once opened, the file is configured with a single
ioctl — see the header and the examples in `test/` for its use.  `write(2)` always
blocks on transmission of a single packet. By default,
`read(2)` will block until the arrival of a packet when it will return,
copying into the supplied buffer a header and then the packet itself.  The
header is an eight-byte unsigned integer, giving the number of nanoseconds
since the epoch, followed by a one-byte unsigned integer giving the pipe
number.

A useful example is `test/one_way`, in which tx sends stdin to rx, which prints
the data to stdout:
```
$ sudo ./rx /dev/nRF24L01:0 # start the receiver
```
```
$ sudo ./tx /dev/nRF24L01:1 # elsewhere, start the transmitter and type
The quick brown fox jumps over the lazy dog.
```
```
$ sudo ./rx /dev/nRF24L01:0
2025-10-10 13:19:36.313 pipe 0: The quick brown fox jumps over t
2025-10-10 13:19:36.319 pipe 0: he lazy dog.\n
```
`test/ping_pong` is a test of both reading from and writing to the
same device.  Starting `pp <device> pong` in one terminal and then
`pp <device> ping` in another will cause the first to wait for a
message before replying with "pong";  likewise the second.

Note that, despite their apparent identical descriptions, the
RFM70 and nRF24L01 are not entirely compatible on-air.  Specifically,
transmitting from an nRF24L01 to an RFM70 with auto-acknowledgement
does not work.  I had some success with using long delays or increased
retransmission counts but would simply recommend avoiding this
configuration.
