#pragma once

#include "esp_err.h"
#include "ulani_ble.h"

/* Shared between ulani_ble.c (GAP/GATT plumbing) and ulani_transfer.c. */

/* Writes with response and blocks until the peer acknowledges. */
esp_err_t ulani_gatt_write(uint16_t val_handle, const void *data, uint16_t len,
                           uint32_t timeout_ms);

uint16_t ulani_op_handle(void);
uint16_t ulani_dat_handle(void);

/*
 * Sends an op-channel frame and, if wait_rsp, blocks for the matching notify.
 * rsp receives the first two bytes of the reply.
 */
esp_err_t ulani_op_exec(const uint8_t *frame, uint16_t len, bool wait_rsp, uint16_t *rsp);

/*
 * Arms the data-channel result latch before a transfer, then waits on it.
 * The device answers a completed (or rejected) image with an 0x02 frame on the
 * data characteristic.
 */
void      ulani_data_result_arm(void);
esp_err_t ulani_data_result_wait(uint32_t timeout_ms, uint16_t *rsp);
bool      ulani_data_result_ready(void);

void ulani_set_state(ulani_state_t s);
void ulani_emit(const ulani_event_t *ev);
bool ulani_transfer_abort_requested(void);
void ulani_transfer_abort_clear(void);
