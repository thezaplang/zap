#!/bin/bash
# Delegate to the Python-based test runner
exec python3 run_tests.py "$@"
