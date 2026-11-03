# Phase 0 network check: connect to an rsync daemon over MacTCP and log the exchange.
cmake_minimum_required(VERSION 3.12)
project(NetTest C CXX)
add_application(NetTest nettest.c CONSOLE)
