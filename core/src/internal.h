// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared by the core's translation units: the public headers plus
// ygopro-core's own (the session reads a few things straight off the engine's
// structs, e.g. each card's current location, the chain, the select lists).
#pragma once
#include "ocgbridge/session.h"
#include "ocgbridge/answers.h"

#include "ocgapi.h"
#include "ocgapi_constants.h"
#include "duel.h"
#include "field.h"
#include "effect.h"
#include "card.h"
