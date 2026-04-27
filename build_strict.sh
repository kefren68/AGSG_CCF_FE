#!/bin/bash

# Wrapper per build.sh che segnala errore se nel log compaiono errori di compilazione

./build.sh

if grep -q "error:" build.log; then
    echo "\n❌ Compilazione fallita: errori trovati nel log!"
    exit 2
else
    echo "\n✅ Compilazione senza errori di compilazione."
    exit 0
fi
