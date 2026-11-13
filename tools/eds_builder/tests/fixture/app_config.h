/* Test input for the scanner: values that come from macros, an enum and constants. */
#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#define APP_VENDOR_ID      0x04D2u          /* 1234 */
#define APP_PRODUCT_NAME   "Fixture " "Drive"
#define ASM_BASE           100
#define ASM_INPUTS         (ASM_BASE + 1)   /* 101 */
#define INPUT_WORDS        6
#define INPUT_BYTES        (INPUT_WORDS * 2) /* 12 */

enum {
    ASM_OUTPUTS = 150,
    ASM_CONFIG,                              /* 151 */
    ASM_HEARTBEAT = 0x00EE                   /* 238 */
};

static const unsigned short kWideInstance = 0x0300; /* 768: needs a 16-bit path segment */

#endif
