% digitalbits-core(1)
% DigitalBits Foundation
%

# NAME

digitalbits-core - Core daemon for DigitalBits network

# SYNOPSYS

digitalbits-core [OPTIONS]

# DESCRIPTION

DigitalBits is a decentralized, federated peer-to-peer network that allows
people to send payments in any asset anywhere in the world
instantaneously, and with minimal fee. `digitalbits-core` is the core
component of this network. `digitalbits-core` is a C++ implementation of
the Stellar Consensus Protocol configured to construct a chain of
ledgers that are guaranteed to be in agreement across all the
participating nodes at all times.

## Configuration file

In most modes of operation, digitalbits-core requires a configuration
file.  By default, it looks for a file called `digitalbits-core.cfg` in
the current working directory, but this default can be changed by the
`--conf` command-line option.  The configuration file is in TOML
syntax.  The full set of supported directives can be found in
`%prefix%/share/doc/digitalbits-core_example.cfg`.

%commands%

# EXAMPLES

See `%prefix%/share/doc/*.cfg` for some example digitalbits-core
configuration files

# FILES

digitalbits-core.cfg
:   Configuration file (in current working directory by default)

# SEE ALSO

<https://developer.digitalbits.io/digitalbits-core/software/admin.html>
:   digitalbits-core administration guide

<https://www.digitalbits.io>
:   Home page of DigitalBits Foundation

# BUGS

Please report bugs using the github issue tracker:\
<https://github.com/digitalbitsorg/digitalbits-core/issues>
