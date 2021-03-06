# Harvester proxy sources
To add unchecked proxies to LiveProxies, you will need proxy list sources. All proxy source files goes into `/etc/liveproxies/scripts` or `./scripts`. Program accepts proxies in following format:
```
setType 508
127.0.0.1:8080
setType 3
127.0.0.1:8888
[::ffff]:8888
```
### setType
`setType` sets type of all proxies below it. Currently, program supports only numbers defined in [Proxylists.h](../ProxyLists.h#L12). Proxy types can be combined, for example, to add HTTP and HTTPS proxies `127.0.0.1` with port `8080` and `2000:2222:3333::4444` with port `8888`, your static file (`.txt` or `.prx`) should look like this:
```
setType 3
127.0.0.1:8080
[2000:2222:3333::4444]:8888
```
Currently program supports following formats of proxy list sources:
 - Script `.cmd`
 - Static file `.txt` or `.prx`
 - URL `.url`

## Script
In order to add proxies from html websites, or to clean proxy lists before pushing them to the program, you should use scripts. You can use any kind of scripting engine as long as it's installed in the server and it's outputting proxy list in format described above to STDOUT. See [BlogspotGeneric.py](../BlogspotGeneric.py) for example python script. **NOTE:** All Blogspot websites are different, so script doesn't always match with blog HTML.

Script file extension is .cmd and it must contain one line of command, for example `python /etc/liveproxies/scripts/test.py`, `mono /etc/liveproxies/scripts/test.exe`.
## Static file
If you want to add static lists of proxies, you should use `.txt` or `.prx` extension files. Just leave proxy list in format described above.
## URL
If remote website is just plain list of proxies in format `IP:PORT`, you should use URL proxy source format. All `.url` files should be in following format:
```
setType 3
http://127.0.0.1/proxylist.txt
```
In this case, program will push all proxies in `http://127.0.0.1/proxylist.txt` with HTTP and HTTPS type.

It is worth noting that LiveProxies will accept proxy even if ending of line contains unnessecary information (except if it cointains ":", a delimiter for IP address and port). Here's a couple of examples:
```
===== Checked With Proxyfire 1.24 Check Report =====	(adds none)
Checked in the USA on a 10sec Timeout					(adds none)
---------------------------------------------------		(adds none)
Check Date:     |  Wed Jun 20 00:33:39 2012				(adds none)
High Anonymous: |  373									(adds none)
----------------------------------------------------	(adds none)
119.246.162.184:8909									(adds 119.246.162.184:8909)
74.115.0.8:80											(adds 74.115.0.8:80)
58.42.247.230:80										(adds 58.42.247.230:80)
```
```
127.0.0.1:8888 (CHECKED WITH JOHN PROXY CHECKER)	(adds 127.0.0.1:8888)
192.168.0.200:80 (CHECKED WITH JOHN PROXY CHECKER)	(adds 192.168.0.200:80)
```
```
Proxy list updated at Sun, 30 Aug 15 21:55:01 +0300 (adds none)
IP address:Port Country-Anonymity(Noa/Anm/Hia)-SSL_support(S)-Google_passed(+) (adds none)
 (adds none)
180.250.196.82:8080 ID-N-S + (adds 180.250.196.82:8080)
27.115.75.114:8080 CN-A - (adds 27.115.75.114:8080)
36.250.75.98:80 CN-H! - (adds 36.250.75.98:80)
```