/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared between the three translation units of this component. Not installed.
 */

#pragma once

#include "matter_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The hook table app_main handed to matter_lock_start(). Never NULL
 *         once the stack is running; the fields inside it may be. */
const matter_lock_hooks_t *matter_lock_hooks(void);

/** @brief Endpoint the door lock lives on. 0 until the endpoint is created. */
uint16_t matter_lock_endpoint(void);

/** @brief Remember whether a controller has provisioned an Aliro reader
 *         identity, so the web UI can say so after a restart. */
void matter_lock_set_reader_configured(bool configured);

#ifdef __cplusplus
}
#endif
