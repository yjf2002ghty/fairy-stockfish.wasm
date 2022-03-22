#!/bin/bash

git log | grep "bench:" | head -n 1 | awk '{print $NF}'
