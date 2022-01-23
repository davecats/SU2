#!/bin/bash
./meson.py build --prefix=`pwd` -Denable-pywrapper=true -Denable-openblas=true #--buildtype=debug
./ninja -C build install
