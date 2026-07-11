/*
 * driver/gpio.h — host-oldali stub (node_queue_sim tesztpad).
 * Csak a node_config.h pin-makróihoz kellenek a GPIO_NUM_x enum értékek.
 */
#ifndef MOCK_DRIVER_GPIO_H
#define MOCK_DRIVER_GPIO_H

typedef enum {
    GPIO_NUM_0 = 0,  GPIO_NUM_1 = 1,   GPIO_NUM_2 = 2,   GPIO_NUM_3 = 3,
    GPIO_NUM_4 = 4,  GPIO_NUM_5 = 5,   GPIO_NUM_6 = 6,   GPIO_NUM_7 = 7,
    GPIO_NUM_8 = 8,  GPIO_NUM_9 = 9,   GPIO_NUM_10 = 10, GPIO_NUM_11 = 11,
    GPIO_NUM_12 = 12, GPIO_NUM_13 = 13, GPIO_NUM_14 = 14, GPIO_NUM_15 = 15,
    GPIO_NUM_16 = 16, GPIO_NUM_17 = 17, GPIO_NUM_18 = 18, GPIO_NUM_19 = 19,
    GPIO_NUM_20 = 20, GPIO_NUM_21 = 21,
} gpio_num_t;

#endif /* MOCK_DRIVER_GPIO_H */
