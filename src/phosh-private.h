/*
 * Copyright (C) 2019,2021 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0+
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#pragma once

#include "keybindings.h"

#include <phosh-private-protocol.h>
#include "glib-object.h"

G_BEGIN_DECLS

#define PHOC_TYPE_PHOSH_PRIVATE (phoc_phosh_private_get_type ())

G_DECLARE_FINAL_TYPE (PhocPhoshPrivate, phoc_phosh_private, PHOC, PHOSH_PRIVATE, GObject)

typedef struct _PhocDesktop PhocDesktop;

PhocPhoshPrivate *phoc_phosh_private_new (void);
bool              phoc_phosh_private_forward_keysym (PhocKeyCombo *combo, uint32_t timestamp);
void              phoc_phosh_private_notify_startup_id (PhocPhoshPrivate                           *self,
                                                        const char                                 *startup_id,
                                                        enum phosh_private_startup_tracker_protocol proto);
void              phoc_phosh_private_notify_launch (PhocPhoshPrivate                           *self,
                                                    const char                                 *startup_id,
                                                    enum phosh_private_startup_tracker_protocol proto);

G_END_DECLS
