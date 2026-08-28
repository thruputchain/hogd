#pragma once

#include "network_config.hh"

namespace hogd {

/// The configuration that results from applying `upgrade` to `config`.
///
/// An upgrade names one parameter; every other parameter keeps the value it
/// already has. A block that changes several carries one upgrade per
/// parameter, so applying them in order yields the configuration the next
/// block executes under.
NetworkConfig apply_config_upgrade(NetworkConfig const &config,
                                   ConfigUpgrade const &upgrade);

} // namespace hogd
