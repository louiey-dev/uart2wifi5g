# TODOs

## IO define

- Flash via USB/UART at dev board
- UART TX : IO8
- UART RX : IO9

## Things TODO

- [x] Seperates .c files according to its function
  - [x] bsp_softap.c
    - [x] bsp_init_softap() : Init SoftAP and run
    - [x] bsp_send_udp() : send data (byte array or text string) via udp
  - [x] bsp_uart_handler.c
    - [x] bsp_uart_init() : Init uart1 which is communicates with external device
    - [x] bsp_uart_rx_handler() : receives incoming uart message/data from external device and send to host via udp
  - [x] main.c
    - [x] init console, SoftAP and uart for external device
