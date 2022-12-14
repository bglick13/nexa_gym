#!/bin/sh

#Convert paths from Windows style to POSIX style
MSYS_BIN=$(echo "/$MSYS_BIN" | sed -e 's/\\/\//g' -e 's/://' -e 's/\"//g')
PATH_DEPS=$(echo "/$PATH_DEPS" | sed -e 's/\\/\//g' -e 's/://' -e 's/\"//g')
TOOLCHAIN_BIN=$(echo "/$TOOLCHAIN_BIN" | sed -e 's/\\/\//g' -e 's/://' -e 's/\"//g')
NEXA_GIT_ROOT=$(echo "/$NEXA_GIT_ROOT" | sed -e 's/\\/\//g' -e 's/://' -e 's/\"//g')
BUILD_TYPE=$(basename $PATH_DEPS)

# Set PATH using POSIX style paths
PATH="$TOOLCHAIN_BIN:$MSYS_BIN:$PATH"

# Verify that required dependencies have been built
CHECK_PATH="$PATH_DEPS/openssl-1.0.2o/libssl.a"
if [ ! -e "$CHECK_PATH" ]
then
	echo OpenSSL dependency is missing.  Please run config-mingw.bat.
	exit -1
fi
CHECK_PATH="$PATH_DEPS/libevent-2.0.22/.libs/libevent.a"
if [ ! -e "$CHECK_PATH" ]
then
	echo LibEvent dependency is missing.  Please run config-mingw.bat.
	exit -1
fi
CHECK_PATH="$PATH_DEPS/miniupnpc/libminiupnpc.a"
if [ ! -e "$CHECK_PATH" ]
then
	echo MiniUPNPC dependency is missing.  Please run config-mingw.bat.
	exit -1
fi
CHECK_PATH="$PATH_DEPS/protobuf-2.6.1/src/.libs/libprotobuf.a"
if [ ! -e "$CHECK_PATH" ]
then
	echo Protobuf dependency is missing.  Please run config-mingw.bat.
	exit -1
fi
CHECK_PATH="$PATH_DEPS/libpng-1.6.36/.libs/libpng.a"
if [ ! -e "$CHECK_PATH" ]
then
	echo LibPNG dependency is missing.  Please run config-mingw.bat.
	exit -1
fi
CHECK_PATH="$PATH_DEPS/qrencode-4.0.2/.libs/libqrencode.a"
if [ ! -e "$CHECK_PATH" ]
then
	echo LibQREncode dependency is missing.  Please run config-mingw.bat.
	exit -1
fi
# In Boost 1.68, the output libraries have different names depending on 32 or 64 bit so check both
if [ "$BUILD_TYPE" = "x86" ]
then
	CHECK_PATH="$PATH_DEPS/boost_1_68_0/bin.v2/libs/chrono/build/gcc-7.3.0/release/link-static/threading-multi/libboost_chrono-mgw73-mt-s-x32-1_68.a"
	if [ ! -e "$CHECK_PATH" ]
	then
		echo Boost 32-bit dependency is missing.  Please run config-mingw.bat.
		exit -1
	fi
fi
if [ "$BUILD_TYPE" = "x64" ]
then
	CHECK_PATH="$PATH_DEPS/boost_1_68_0/bin.v2/libs/chrono/build/gcc-7.3.0/release/link-static/threading-multi/libboost_chrono-mgw73-mt-s-x64-1_68.a"
	if [ ! -e "$CHECK_PATH" ]
	then
		echo Boost 64-bit dependency is missing.  Please run config-mingw.bat.
		exit -1
	fi
fi
CHECK_PATH="$PATH_DEPS/Qt-5.9.9/5.9.9/lib/libQt5Core.a"
if [ ! -e "$CHECK_PATH" ]
then
	echo Qt dependency is missing.  Please run config-mingw.bat.
	exit -1
fi

CHECK_PATH="/d/msys64/mingw64/lib/libzmq.a"
if [ ! -e "$CHECK_PATH" ]
then
	echo Lib zmq dependency is missing
	exit -1
fi

#If autogen is set, then configure MUST be set
if [ "$AUTOGEN" = "YES" ]
then
	CONFIGURE=YES
fi

# Build Nexa
cd "$NEXA_GIT_ROOT"

#define and export BOOST_ROOT prior to any calls that require
#executing ./configure (this may include `make clean`) depending on current system state
export BOOST_ROOT="$PATH_DEPS/boost_1_68_0"


#if the clean parameter was passed call clean prior to make
if [ "$CLEAN_BUILD" = "YES" ]; then
	echo 'Cleaning build...'
	make clean
	make distclean

        AUTOGEN=YES
        CONFIGURE=YES
fi


#autogen (improve build speed if this step isn't necessary)
if [ "$AUTOGEN" = "YES" ]
then
	echo 'Running autogen...'
	./autogen.sh
fi

# NOTE: If you want to run tests (make check and rpc-tests) you must
#       1. Have built boost with the --with-tests flag (in config-mingw.bat)
#       2. Have built a Hexdump equivalent for mingw (included by default in install-deps.sh)
if [ "$CONFIGURE" = "YES" ]; then
	echo 'Running configure...'
	# By default build without tests
	DISABLE_TESTS="--disable-tests"
	# However, if the --check argument was specified, we will run "make check"
	# which means we need to configure for build with tests enabled
	if [ -n "$ENABLE_TESTS" ]; then
		echo 'Enabling tests in ./configure command'
		DISABLE_TESTS=
	fi
	
	# Uncomment below to build release
	ENABLE_DEBUG=
	# Uncomment below to build debug
	#ENABLE_DEBUG="--enable-debug"

	CPPFLAGS="-I$PATH_DEPS/db-5.3.21/build_unix \
	-I/d/msys64/mingw64/include \
	-I$PATH_DEPS/openssl-1.0.2o/include \
	-I$PATH_DEPS/libevent-2.0.22/include \
	-I$PATH_DEPS \
	-I$PATH_DEPS/protobuf-2.6.1/src \
	-I$PATH_DEPS/libpng-1.6.36 \
	-I$PATH_DEPS/qrencode-4.0.2 \
	-I$PATH_DEPS/gmp-6.2.0+dfsg" \
	LDFLAGS="-L$PATH_DEPS/db-5.3.21/build_unix \
	-L/d/msys64/mingw64/lib \
	-L$PATH_DEPS/openssl-1.0.2o \
	-L$PATH_DEPS/libevent-2.0.22/.libs \
	-L$PATH_DEPS/miniupnpc \
	-L$PATH_DEPS/protobuf-2.6.1/src/.libs \
	-L$PATH_DEPS/libpng-1.6.36/.libs \
	-L$PATH_DEPS/Qt-5.9.9/5.9.9/lib \
	-L$PATH_DEPS/qrencode-4.0.2/.libs \
	-L$PATH_DEPS/gmp-6.2.0+dfsg/.libs" \
	BOOST_ROOT="$PATH_DEPS/boost_1_68_0" \
	./configure \
	$ENABLE_DEBUG \
	--disable-upnp-default \
        --with-sodium \
	$DISABLE_TESTS \
	--with-qt-incdir="$PATH_DEPS/Qt-5.9.9/5.9.9/include" \
	--with-qt-libdir="$PATH_DEPS/Qt-5.9.9/5.9.9/lib" \
	--with-qt-plugindir="$PATH_DEPS/Qt-5.9.9/5.9.9/plugins" \
	--with-qt-bindir="$PATH_DEPS/Qt-5.9.9/5.9.9/bin" \
	--with-protoc-bindir="$PATH_DEPS/protobuf-2.6.1/src"
fi

# Default to 2 threads if nothing has been set
if [ -z $MAKE_CORES ]; then
        MAKE_CORES=-j2
fi
echo "making with $MAKE_CORES threads"
make $MAKE_CORES

# Optinally run make check tests (REVISIT: currently not working due to issues with python3 scripts)
# NOTE: This will only function if you have built BOOST with tests enabled
#       and have the correct version of python installed and in the Windows PATH
#if [ -n "$ENABLE_TESTS" ]; then
#	# Skip make check for now since it doesn't work on Windows
#	#echo 'Running make check tests'
#	#make check
#	
#	#echo 'Running qa tests'
#	#./qa/pull-tester/rpc-tests.py --win
#fi

# Strip symbol tables
if [ -n "$STRIP" ]; then
	echo 'Stripping exeutables'
	strip src/nexa-tx.exe
	strip src/nexa-cli.exe
	strip src/nexa-miner.exe
	strip src/nexad.exe
	strip src/qt/nexa-qt.exe
fi
