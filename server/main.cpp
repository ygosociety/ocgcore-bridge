// SPDX-License-Identifier: AGPL-3.0-or-later
// ocgcore-bridge: one duel per process, newline-delimited JSON over stdin /
// stdout (protocol v1, docs/protocol.md).
//
//   ocgcore-bridge [--scripts DIR] [--script-dirs SUB,SUB...] [--cdb FILE]...
//                  [--cards-json FILE [--setcodes FILE] [--archetypes FILE]]
#include "server.h"

int main(int argc, char** argv) { return ocgbridge::server::run(argc, argv); }
