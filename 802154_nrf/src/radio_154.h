#ifndef RADIO_154_H_
#define RADIO_154_H_

#include <stddef.h>
#include <stdint.h>

int radio_154_init(void);
int transmit_802_15_4(uint8_t *buf, size_t len);

#endif /* RADIO_154_H_ */
