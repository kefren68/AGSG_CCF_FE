#!/bin/sh
# 1. Definizione percorsi (FE dentro bin)
FE_DIR="/sdcard/bin/AGSG_CCF_FE"
BIN_NAME="launcher"
LIBS_DIR="/sdcard/bin/libs"

# 2. Preparazione
cp $FE_DIR/$BIN_NAME /tmp/launcher_bin
chmod +x /tmp/launcher_bin
export LD_LIBRARY_PATH=$LIBS_DIR:$LD_LIBRARY_PATH

# 3. Congela il menu originale
killall -STOP game 2>/dev/null

# 4. Avvia il Launcher
cd $FE_DIR
/tmp/launcher_bin

# 5. Ripristino
killall -CONT game 2>/dev/null


