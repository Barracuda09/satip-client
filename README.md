# satipclient
SAT>IP Client FORK from: https://code.google.com/p/satip/

Supported options in /etc/vtuners.conf:
- tcpdata:1 - uses TCP instead of UDP for the connection with the satip server
- force_plts:1 - forces sending plts=on as part of the satip request
- fe:X - send fe=X as part of the satip request to force a specific adapter (useful on multiple satellite connections on different adapters)
- ipaddr - the ip address of the satip server
- port - the port of the satip server

Supported Startup arguments in /etc/init.d/satipclient:
- -m <debug_mask>  Used for debugging (Add together)
                     MSG_MAIN   1
                     MSG_NET    2
                     MSG_HW     4
                     MSG_SRV    8
                     MSG_DATA  16
- -l <log_level>   Log level: (default: 1)
                     0: None
                     1: Error
                     2: Warning
                     3: Info
                     4: Debug
- -y               Use syslog instead of STDERR for logging

Example: start-stop-daemon -S -b -x /usr/bin/satipclient -- -m 3 -l 4 -y

## Building

```
autoreconf --install
./configure
make
```

To compile for e.g. VU Solo 4K (ARMv7 architecture):

```
./configure --with-boxtype=vusolo4k --host=arm-linux-gnueabihf
make
```

Make sure you have g++ and cross-compilation tools for `arm-linux-gnueabihf` installed 
(e.g. using `sudo apt-get install g++-arm-linux-gnueabihf`), otherwise you'll end up with a binary for your 
host's architecture.
