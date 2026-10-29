// Copyright (C) 2026 SunjooAn. SPDX-License-Identifier: GPL-2.0-or-later.
#pragma once
#include <obs.h>
// Only edits filter settings on Apply. Never seeks, pauses, or renders the parent.
void cue_editor_open(obs_source_t *filter,const char *parentUuid,const char *localFile,double inSeconds,double outSeconds);
void cue_editor_shutdown();
