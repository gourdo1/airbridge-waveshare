# ResMed UART link test (fork)

A read-only check of the AirBridge to AirSense serial link at bootloader
speeds, before trusting it with a firmware or bootloader write. Added for the
`waveshare-s3-lcd154-i2cpads` wiring, whose open-drain TX relies on pull-ups
and may not keep up at 460800 baud.

## What it does

1. Same admission as a dump: the AirSense must be in standby, with no USB
   storage handoff or other update running. No SD card is needed.
2. Claims the UART, pauses live streams and checks the device identity.
3. Enters the bootloader (`P S #BLL 0001`) at 57600 baud.
4. For 115200 or 460800, switches speed with `P S #BDD 0001` / `0002`, as a
   dump or install does.
5. Sends `G S #BLS` (bootloader status) `count` times, waiting up to 300 ms
   for each reply.
6. Stops sending and lets the bootloader time out back to the application,
   then confirms the application is running with the same identity.

It never erases, writes or reads flash. It stops early after 4 failures in a
row, well inside the bootloader's ~2 s inactivity timeout.

The stock AirSense 10 bootloader leaves about 5 s after entry even while it
keeps answering status reads (measured at both 57600 and 460800; a real write
is unaffected because it starts erasing immediately). The test therefore
stops 4.5 s after entering the bootloader and reports `stopped: bootloader
time window`, which is not an error. That is roughly 500 exchanges at 57600
and 2000+ at 460800; repeat runs to accumulate more.

## Running it

From the web UI console, the USB console, or the TCP command port (in
PuTTY use connection type Raw; Telnet mode prefixes the first line with
negotiation bytes, so it is forwarded to the AirSense instead):

```
$FLASH LINKTEST 460800 2000     start (count defaults to 2000, range 10..20000)
$FLASH LINKTEST                 show the last result
$FLASH STATUS                   phase and error of the running operation
$FLASH CANCEL                   stop early
```

Run 57600 first as a baseline, then 460800 (and 115200 if 460800 fails).

## Reading the result

| Field | Meaning |
|---|---|
| `ok` | Reply received, CRC valid, bootloader state 1 or 2 |
| `no_reply` | No valid reply in 300 ms. Usually AirBridge to AirSense (TX) corruption: the bootloader drops frames with a bad CRC |
| `rx_crc_errors` | AirSense to AirBridge (RX) frames that failed CRC |
| `bad_reply` | Error reply or unexpected state value |
| `first_failure` | Exchange number of the first failure |
| `stopped` | The 4.5 s bootloader window ended the run before `count`; not an error |

2000 exchanges move about as many bytes each way as a 16 KB bootloader write.
Any failure at a given speed means: do not write at that speed over this
wiring. Zero failures is good evidence, not proof; the write uses longer
frames (~260 bytes) than this test (~20 bytes).
