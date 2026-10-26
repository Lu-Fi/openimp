#!/bin/bash
# imgfx is built together with apitest
exec "$(dirname "$0")/../apitest/build.sh" "$@"
