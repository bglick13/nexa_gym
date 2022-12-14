# Dependencies

These are the dependencies currently used by Bitcoin Unlimited. You can find instructions for installing them in the `build-*.md` file for your platform.

| Dependency | Version used | Minimum required | CVEs | Shared | [Bundled Qt library](https://doc.qt.io/qt-5/configure-options.html) |
| --- | --- | --- | --- | --- | --- |
| Berkeley DB | [5.3.28](http://www.oracle.com/technetwork/database/database-technologies/berkeleydb/downloads/index.html) | 5.3.x | No |  |  |
| Boost | [1.71.0](http://www.boost.org/users/download/) | [1.55.0](https://github.com/BitcoinUnlimited/BitcoinUnlimited/pull/1562) | No |  |  |
| ccache | [3.4.2](https://ccache.samba.org/download.html) |  | No |  |  |
| D-Bus | [1.10.19](https://cgit.freedesktop.org/dbus/dbus/tree/NEWS?h=dbus-1.10) |  | No | Yes |  |
| Expat | [2.4.1](https://libexpat.github.io/) |  | No | Yes |  |
| fontconfig | [2.12.1](https://www.freedesktop.org/software/fontconfig/release/) |  | No | Yes |  |
| FreeType | [2.11.0](http://download.savannah.gnu.org/releases/freetype) |  | No |  |  |
| GCC |  | [5+](https://gcc.gnu.org/) |  |  |  |
| HarfBuzz-NG |  |  |  |  |  |
| libevent | [2.1.8-stable](https://github.com/libevent/libevent/releases) | 2.0.22 | No |  |  |
| libpng |  |  |  |  | [Yes](https://github.com/bitcoin/bitcoin/blob/master/depends/packages/qt.mk) |
| MiniUPnPc | [2.0.20180203](http://miniupnp.free.fr/files) |  | No |  |  |
| OpenSSL | [1.1.1m](https://www.openssl.org/source) |  | Yes |  |  |
| PCRE |  |  |  |  | [Yes](https://github.com/bitcoin/bitcoin/blob/master/depends/packages/qt.mk#L76) |
| protobuf | [3.14.9](https://github.com/google/protobuf/releases) |  | No |  |  |
| Python (tests) |  | [3.6](https://www.python.org/downloads) |  |  |  |
| qrencode | [3.4.4](https://fukuchi.org/works/qrencode) |  | No |  |  |
| Qt | [5.15.2](https://download.qt.io/official_releases/qt/) | [5.9.5](https://github.com/bitcoin/bitcoin/issues/20104) | No |  |  |
| XCB |  |  |  |  | [Yes](https://github.com/bitcoin/bitcoin/blob/master/depends/packages/qt.mk) (Linux only) |
| xkbcommon |  |  |  |  | [Yes](https://github.com/bitcoin/bitcoin/blob/master/depends/packages/qt.mk) (Linux only) |
| ZeroMQ | [4.3.1](https://github.com/zeromq/libzmq/releases) |  | Yes |  |  |
| zlib | [1.2.11](http://zlib.net/) |  |  |  | No |
