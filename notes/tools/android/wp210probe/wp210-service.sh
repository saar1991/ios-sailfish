#!/system/bin/sh
# wp210 probe kit, Magisk service.d: after boot, samples and event-driven full dumps (read-only)
(
  . /data/adb/wp210/lib.sh
  [ -e $W/stop ] && exit 0
  while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 1; done
  smc_start
  REASON=booted; key; full booted
  START=$(up | cut -d. -f1); LSUSP=$(susp); LSCR=$(screen); OFF=0; F60=0; F180=0
  DUR=$(cat $W/duration 2>/dev/null || echo 3600)
  note "service running for $DUR s"
  while :; do
    NOW=$(up | cut -d. -f1); EL=$((NOW - START))
    [ -e $W/stop ] && { note "stop file"; break; }
    [ $EL -gt $DUR ] && { note "duration reached"; break; }
    S=$(susp); SC=$(screen)
    if [ "$S" != "$LSUSP" ]; then note "suspend count $LSUSP -> $S"; REASON=resume; key; full resume$S; LSUSP=$S; fi
    if [ "$SC" != "$LSCR" ]; then note "screen $LSCR -> $SC"; REASON=screen; key; LSCR=$SC; if [ "$SC" = 0 ]; then OFF=$NOW; else OFF=0; fi; fi
    if [ $OFF -gt 0 ] && [ $((NOW - OFF)) -ge 20 ]; then REASON=idle; key; full idle; OFF=0; fi
    if [ $F60 = 0 ] && [ $EL -ge 60 ]; then full t60; F60=1; fi
    if [ $F180 = 0 ] && [ $EL -ge 180 ]; then full t180; F180=1; fi
    REASON=tick; key
    if [ $EL -lt 180 ]; then sleep 5; else sleep 30; fi
  done
  smc_stop
  REASON=end; key; full end
  note "service end"
) > /dev/null 2>&1 &
