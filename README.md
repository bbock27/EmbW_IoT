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
* 
