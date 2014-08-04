#!/bin/bash

TAG="[Pods-setup.sh]:"

# Setup directory variables
LLVM_DIR="$( cd "$( dirname "$0" )"; pwd )"
PODS_DIR="$( cd "$LLVM_DIR"; cd ..; pwd )"
CLANG_DIR="$PODS_DIR/multicompiler-clang"
TOOLS_DIR="$LLVM_DIR/tools/clang"
BUILD_DIR="$LLVM_DIR/build"
PODSPEC="$TOOLS_DIR/multicompiler-clang.podspec"

# Directory configuration
if [ -d $TOOLS_DIR ]; then
	if [ -f $PODSPEC ]; then
		echo "$TAG '$TOOLS_DIR' ready..."
	else
		# tools/clang is incomplete. attempt to copy
		if [ -d $CLANG_DIR ]; then 
			# Move clang into tools directory
			echo "$TAG Moving '$CLANG_DIR' to '$TOOLS_DIR'..."
			echo "rm -rf $TOOLS_DIR"
			rm -rf $TOOLS_DIR
			echo "mv $CLANG_DIR $TOOLS_DIR"
			mv $CLANG_DIR $TOOLS_DIR
		else
			echo "$TAG '$CLANG_DIR' directory does not exist."
			echo "$TAG Please run 'pod install' from your app's top directory."
			echo "$TAG Exiting..."
			exit -1
		fi
	fi
else
	if [ -d $CLANG_DIR ]; then
		echo "$TAG Moving '$CLANG_DIR' to '$TOOLS_DIR'..."
		echo "mv $CLANG_DIR $TOOLS_DIR"
		mv $CLANG_DIR $TOOLS_DIR
	else
		echo "$TAG '$CLANG_DIR' directory does not exist."
		echo "$TAG Please run 'pod install' from your app's top directory."
		echo "$TAG Exiting..."
		exit -1
	fi
fi

# Make 'build' directory for clang compile prefix
if [ ! -d $BUILD_DIR ]; then
	echo "$TAG Creating '$BUILD_DIR' prefix for clang installation..."
	echo "mkdir $BUILD_DIR"
	mkdir $BUILD_DIR
fi

# Get argument flags
CONFIG=1
MAKE=1
INSTALL=1

for arg in "$@"; do
	case $arg in
		-c|--config) CONFIG=1 ;;
		-m|--make) MAKE=1 ;;
		-i|--install) INSTALL=1 ;;
		*);;
	esac
done

if [ $CONFIG -eq 0 ]; then
	echo "$TAG -c (--config) flag not given: stopping..."
	echo "$TAG Please './configure [flags]', 'make', and 'make install' the multicompiler."
	echo "$TAG Exiting..."
	exit 1
fi

echo "$TAG Starting configuration of multicompiler..."
pushd "$LLVM_DIR"
echo "./configure --prefix=\"$BUILD_DIR\" --enable-optimized"
./configure --prefix="$BUILD_DIR" --enable-optimized
RET=$?

if [ $RET -eq 0 ]; then
	echo "$TAG Multicompiler configured successfully."
else
	echo "$TAG Error configuring multicompiler. (exit code $RET)"
	echo "$TAG Exiting..."
	exit -1
fi

if [ $MAKE -eq 0 ]; then
	echo "$TAG -m (--make) flag not given: stopping..."
	echo "$TAG Please 'make' and 'make install' the multicompiler."
	echo "$TAG Exiting..."
	exit 2
fi

echo "$TAG Starting make of multicompiler... (this may take some time)"
echo "make -j 3"
make -j 3
RET=$?

if [ $RET -eq 0 ]; then
	echo "$TAG Multicompiler made successfully."
else
	echo "$TAG Error making multicompiler. (exit code $RET)"
	echo "$TAG Exiting..."
	exit -1
fi

if [ $INSTALL -eq 0 ]; then
	echo "$TAG -i (--install) flag not given: stopping..."
	echo "$TAG Please 'make install' to install into $BUILD_DIR."
	echo "$TAG Exiting..."
	exit 3
fi

echo "$TAG Starting make install... "
echo "make install"
make install
RET=$?

if [ $RET -eq 0 ]; then
	echo "$TAG Multicompiler installed successfully."
else
	echo "$TAG Error installing multicompiler. (exit code $RET)"
	echo "$TAG Exiting..."
	exit -1
fi

echo "$TAG Finishing..."
popd

exit 0

# Set application compiler flags
#sed -i.bak s/OTHER_CFLAGS\.\*$/OTHER_CFLAGS\ =\ \(\"\-fdiversify\"\,\"\-frandom-seed=0xcafe\"\,/g project.pbxproj

# Setup random seed generation on compile.

