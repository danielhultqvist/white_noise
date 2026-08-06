#!/usr/bin/env bash
set +e
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc .
echo "Compiling done."
