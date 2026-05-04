# EmbW_IoT
## Introduction
The goal of this project is to create a 802.15.4 network bridge over wifi using nrf54l15s. Two nRF54L15 boards on the same network can communicate with each other using the 802.15.4 protocol, while two boards on different networks must communicate via some other method, for example using wifi. In this project, we have four total boards: two sets of two, with each set connected to a different network. One board in each set is also connected to a daughter board to allow for wifi communication between them. The wifi-connected boards can send and recieve 802.15.4 packets to and from the other board on their networks. The wifi-connected boards can also wrap and unwrap 802.15.4 packets into and from IP packets, and send them to a server which both wifi boards are connected to. This allows packets to be sent to any of the four boards, and make it appear as though all boards are connected to the same network.

## Hardware Details
We use four nRF54L15 boards, and two nRF7002 daughter boards.
#### nRF54L15
* 128 MHz Arm Cortex-M33
* 1.5MB NVM and 256 KB RAM
* Ultra-low-power multiprotocol 2.4GHz Radio
* more details here: https://www.nordicsemi.com/Products/nRF54L15
#### nRF7002
* 2.5 GHz and 5 GHz dual band
* low power and secure Wi-Fi
* coexistence with Bluetooth LE
* more details here: https://www.nordicsemi.com/Products/nRF7002
#### Power
#### RF specs

## Software Environment
#### Firmware
* nRF Connect SDK Toolchain for VS Code v3.2.1
* Zephyr RTOS
* Build system and board configuration

## Reproducibility guide
* picture of the setup:
* Toolchain installation guide: https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation/install_ncs.html
#### For the wifi boards
* Open "network_attached_nrf" as an application.
* In "network_attached_nrf\prj.conf" set "CONFIG_BRIDGE_RELAY_HOST" to be the ip address of the server hosting the bridge.
* Build instructions:
  *   Set "Board Target" to nrf54l15dk\nrf54l15\cpuapp"
  *   Add an extra Cmake argument. The argument should be "-DSHIELD=nrf7002eb2"
  * Flash this application to both wifi boards
#### For the non-wifi boards (802.15.4 connection only)
* Open "802154_nrf" as an application
* In the build configuration, set "Board Target" to nrf54l15dk\nrf54l15\cpuapp"
* Flash this application to both non-wifi boards.
* set `src_extended_addr` and `dst_extended_addr`, for each of the non-wifi boards such that `src_extended_addr` is the same as `dst_extended_addr` for the other.
#### In both applications
* In both applications, there is a script called "radio_154.c"
* In prj.conf, set CONFIG_BRIDGE_15_4_CHANNEL for all the boards such that the two boards on the same network share a channel
 * That is, each wifi-802.15.4 pair shares a channel, but the two pairs should have different channels. 
* The boards that will be communicating over 802.15.4 must have the same channel
### Testing/Measurement
* Open up the VCOM recieving the data from the board (we did so in VSCode)
* For each board, there should be messages stating when the WiFi connection request was sent to the server, messages while it is scanning, and a message when it is connected to the bridge server.
* The output should look something like this
```
[00:00:08.191,508] <inf> main: State: COMPLETED
[00:00:08.191,518] <inf> main: Mode: STATION
[00:00:08.191,529] <inf> main: Link Mode: WIFI 6 (802.11ax/HE)
[00:00:08.191,545] <inf> main: SSID: Ipsk
[00:00:08.191,555] <inf> main: Band: 2.4GHz
[00:00:08.191,559] <inf> main: Channel: 1
[00:00:08.191,569] <inf> main: Security: WPA2-PSK
[00:00:08.191,579] <inf> main: MFP: Optional
[00:00:08.191,583] <inf> main: RSSI: -60
[00:00:09.082,780] <inf> net_dhcpv4: Received: [Board IP]
[00:00:09.082,928] <inf> main: DHCP bound: [Board IP]
[00:00:09.082,951] <inf> bridge: bridge_start: relay=[Server IP:Port] tunnel_id='default' channel=[channel number] pan=0xbeef
[00:00:09.083,007] <inf> radio_154: radio entered rx state
[00:00:09.083,562] <inf> bridge: bridge: radio_to_tunnel_thread + tunnel_to_radio_thread up
[00:00:09.083,830] <inf> tunnel: tunnel connection thread started
[00:00:09.084,000] <inf> tunnel: dialing relay [Server IP:Port]
```
