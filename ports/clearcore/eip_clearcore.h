/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP ClearCore port: optional application settings. */
#ifndef EIP_CLEARCORE_H_
#define EIP_CLEARCORE_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Host name reported in TCP/IP Interface attribute 6 (up to 64 characters).
 * lwIP on ClearCore has no host name of its own, so without this the
 * attribute reads empty. The string is copied. Call it at start-up with the
 * stored name, and again from apply_interface when a scanner sets attr 6:
 * the adapter rereads this name on every interface refresh, so a set value
 * that is not stored here reverts. A longer name is cut to 64 characters. */
void eip_clearcore_set_host_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* EIP_CLEARCORE_H_ */
