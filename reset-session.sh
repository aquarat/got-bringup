#!/bin/bash
# Tear down a muvm/Steam session completely, including the debris a forced
# kill leaves behind.
#
# WHY THIS EXISTS
# ---------------
# autorun.sh's kill_muvm() only kills the `muvm` process. That is enough for a
# clean exit and not enough after a forced one, which strands:
#
#   /run/user/1000/muvm.lock   muvm opens this WITHOUT O_CREAT, so its mere
#                              presence makes muvm behave as a client and try
#                              to connect to a server that is not there. Every
#                              later launch then fails with "could not connect
#                              to muvm server", which points at the socket and
#                              not at the lock. This one cost an hour.
#   /run/user/1000/krun/       the server socket for the dead VM
#   ~/.steam/steam.pid|pipe    Steam's single-instance IPC, holding guest PIDs
#   passt                      muvm's network helper, orphaned and still live
#   VM:fedrat <defunct>        the VM zombie
#   /dev/shm/u1000-Shm_*       26 MB Wine segments, ~1700 of them seen, 9.3 GB,
#                              accumulated over days and never reclaimed
#   stale X windows            Steam/steamwebhelper/FEXInterpreter from the
#                              dead guest, which confuse window detection
#
# Safe to run when nothing is going: everything here is recreated on demand.
set -u
say() { echo "[reset] $*"; }

pids=$(ps -eo pid,args --no-headers | awk '$2=="/bin/bash" && $3 ~ /autorun\.sh$/ {print $1}')
[ -n "$pids" ] && { say "killing harness: $pids"; kill $pids 2>/dev/null; sleep 3; }

mp=$(ps -eo pid,args --no-headers | awk '$2=="/usr/bin/muvm"{print $1}')
[ -n "$mp" ] && { say "killing muvm: $mp"; kill $mp 2>/dev/null; sleep 6; }

pp=$(ps -eo pid,comm --no-headers | awk '$2=="passt"{print $1}')
[ -n "$pp" ] && { say "killing orphaned passt: $pp"; kill $pp 2>/dev/null; sleep 2; }

for f in /run/user/1000/muvm.lock ~/.steam/steam.pid ~/.steam/steam.pipe; do
    [ -e "$f" ] && { say "removing $f"; rm -f "$f"; }
done
[ -d /run/user/1000/krun ] && { say "removing /run/user/1000/krun"; rm -rf /run/user/1000/krun; }

# Only safe once no Wine process can be holding them.
if ! ps -eo comm --no-headers | grep -qiE "wine|steam|FEX"; then
    n=$(ls /dev/shm/u1000-Shm_* 2>/dev/null | wc -l)
    [ "$n" -gt 0 ] && { say "removing $n orphaned Wine shm segments"; rm -f /dev/shm/u1000-Shm_*; }
fi

for w in $(DISPLAY=:0 xdotool search --name "Steam|steamwebhelper|FEXInterpreter" 2>/dev/null); do
    n=$(DISPLAY=:0 xdotool getwindowname "$w" 2>/dev/null)
    case "$n" in
      Steam|steamwebhelper|FEXInterpreter)
        say "destroying stale window $w ($n)"; DISPLAY=:0 xdotool windowkill "$w" 2>/dev/null ;;
    esac
done

say "checking muvm can boot"
if timeout 120 /usr/bin/muvm --mem=4096 -- /bin/true >/dev/null 2>&1; then
    say "muvm OK"
else
    say "muvm STILL BROKEN -- do not start a run, investigate first"
    exit 1
fi
say "done"
