// SPDX-License-Identifier: AGPL-3.0-or-later
// The protocol v1 server as a library: ocgcore-bridge's own main() is
// server::run(argc, argv). A program that links ocgbridge_server can pass
// Extensions to register its own queries, default answers and message
// filters on the session, and still speak the same protocol
// (docs/extending.md).
#pragma once
#include <functional>

#include <nlohmann/json.hpp>

#include "ocgbridge/session.h"

namespace ocgbridge::server {

struct Extensions {
    // Once, after the command line's card data is loaded and before the first
    // request. What it registers on the session stays for every duel (and is
    // copied into forks).
    std::function<void(Session&)> setup;
    // After every successful init, with the request (its unknown fields
    // included, so an extension can read options of its own).
    std::function<void(Session&, const nlohmann::json& init)> on_init;
};

// Runs the server on stdin / stdout until exit or end of input.
int run(int argc, char** argv, const Extensions& ext = {});

} // namespace ocgbridge::server
